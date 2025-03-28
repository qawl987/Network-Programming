#include <ctype.h>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <queue>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>

using namespace std;

class ProcessExecutor {
  public:
    struct ProcessConfig {
        vector<string> arguments;
        array<int, 2> input_pipe = {STDIN_FILENO, STDOUT_FILENO};
        int output_fd = STDOUT_FILENO;
        int error_fd = STDERR_FILENO;
    };

    static void run(const ProcessConfig &config) {
        if (handleBuiltins(config)) {
            return;
        }

        pid_t pid = createChildProcess();
        if (pid != 0) {
            cleanupParentResources(config);
            waitForChildIfNeeded(pid, config);
            return;
        }

        setupChildProcessIO(config);
        executeExternalCommand(config);
    }

  private:
    static bool handleBuiltins(const ProcessConfig &config) {
        const string &cmd = config.arguments[0];

        if (cmd == "exit") {
            exit(0);
        }

        if (cmd == "setenv") {
            setenv(config.arguments[1].c_str(), config.arguments[2].c_str(), 1);
            return true;
        }

        if (cmd == "printenv") {
            if (const char *env = getenv(config.arguments[1].c_str())) {
                cout << env << '\n';
            }
            return true;
        }

        return false;
    }

    static pid_t createChildProcess() {
        pid_t pid;
        while ((pid = fork()) == -1) {
            if (errno == EAGAIN) {
                wait(nullptr);
            }
        }
        return pid;
    }

    static void cleanupParentResources(const ProcessConfig &config) {
        if (config.input_pipe[0] != STDIN_FILENO) {
            close(config.input_pipe[0]);
            close(config.input_pipe[1]);
        }

        struct stat fd_stat;
        fstat(config.output_fd, &fd_stat);

        if (config.output_fd != STDOUT_FILENO && S_ISREG(fd_stat.st_mode)) {
            close(config.output_fd);
        }
    }

    static void waitForChildIfNeeded(pid_t pid, const ProcessConfig &config) {
        struct stat fd_stat;
        fstat(config.output_fd, &fd_stat);

        if (!S_ISFIFO(fd_stat.st_mode)) {
            waitpid(pid, nullptr, 0);
        }
    }

    static void setupChildProcessIO(const ProcessConfig &config) {
        dup2(config.input_pipe[0], STDIN_FILENO);
        dup2(config.output_fd, STDOUT_FILENO);
        dup2(config.error_fd, STDERR_FILENO);
    }

    static void executeExternalCommand(const ProcessConfig &config) {
        auto argv = prepareCommandArguments(config);

        if (execvp(argv[0], argv.get())) {
            if (errno == ENOENT) {
                cerr << "Unknown command: [" << argv[0] << "].\n";
                exit(0);
            }
        }
    }

    static unique_ptr<char *[]>
    prepareCommandArguments(const ProcessConfig &config) {
        auto args = make_unique<char *[]>(config.arguments.size() + 1);
        for (size_t i = 0; i < config.arguments.size(); i++) {
            args[i] = strdup(config.arguments[i].c_str());
        }
        args[config.arguments.size()] = nullptr;
        return args;
    }
};

class PipeManager {
    unordered_map<int, array<int, 2>> active_pipes;

  public:
    void createPipe(int pipe_id) {
        array<int, 2> pipe_fds;
        while (pipe(pipe_fds.data()) == -1) {
            if (errno == EMFILE || errno == ENFILE) {
                wait(nullptr);
            }
        }
        fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC);
        fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC);
        active_pipes.emplace(pipe_id, pipe_fds);
    }

    bool hasPipe(int pipe_id) const { return active_pipes.count(pipe_id) > 0; }

    array<int, 2> getPipe(int pipe_id) const {
        return active_pipes.at(pipe_id);
    }

    void removePipe(int pipe_id) { active_pipes.erase(pipe_id); }

    void shiftPipeNumbers() {
        unordered_map<int, array<int, 2>> new_pipes;
        for (const auto &[id, fds] : active_pipes) {
            new_pipes.emplace(id - 1, fds);
        }
        active_pipes = move(new_pipes);
    }
};

class CommandParser {
    stringstream input_stream;
    vector<string> current_args;
    PipeManager &pipe_manager;

  public:
    CommandParser(const string &line, PipeManager &pm)
        : input_stream(line), pipe_manager(pm) {}

    void processCommands() {
        string token;

        while (input_stream >> token) {
            if (isOperator(token[0])) {
                executeCurrentCommand(token);
            } else {
                current_args.push_back(token);
            }
        }

        executeRemainingCommand();
    }

  private:
    bool isOperator(char c) const { return c == '|' || c == '!' || c == '>'; }

    void executeCurrentCommand(const string &operator_token) {
        ProcessExecutor::ProcessConfig config;
        config.arguments = move(current_args);
        current_args.clear();

        setupInputPipe(config);
        handleOperator(operator_token, config);

        ProcessExecutor::run(config);

        if (operator_token != "|") {
            pipe_manager.shiftPipeNumbers();
        }
    }

    void setupInputPipe(ProcessExecutor::ProcessConfig &config) {
        if (pipe_manager.hasPipe(0)) {
            config.input_pipe = pipe_manager.getPipe(0);
            pipe_manager.removePipe(0);
        }
    }

    void handleOperator(const string &op,
                        ProcessExecutor::ProcessConfig &config) {
        if (op[0] == '>') {
            handleRedirection(config);
        } else {
            handlePiping(op, config);
        }
    }

    void handleRedirection(ProcessExecutor::ProcessConfig &config) {
        string filename;
        input_stream >> filename;
        config.output_fd = open(filename.c_str(),
                                O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0664);
    }

    void handlePiping(const string &op,
                      ProcessExecutor::ProcessConfig &config) {
        int pipe_id = op.size() > 1 ? stoi(op.substr(1)) : 0;

        if (!pipe_manager.hasPipe(pipe_id)) {
            pipe_manager.createPipe(pipe_id);
        }

        auto pipe_fds = pipe_manager.getPipe(pipe_id);

        if (op[0] == '|') {
            config.output_fd = pipe_fds[1];
        } else if (op[0] == '!') {
            config.output_fd = pipe_fds[1];
            config.error_fd = pipe_fds[1];
        }
    }

    void executeRemainingCommand() {
        if (!current_args.empty()) {
            ProcessExecutor::ProcessConfig config;
            config.arguments = move(current_args);

            if (pipe_manager.hasPipe(0)) {
                config.input_pipe = pipe_manager.getPipe(0);
                pipe_manager.removePipe(0);
            }

            ProcessExecutor::run(config);
            pipe_manager.shiftPipeNumbers();
        }
    }
};

int main() {
    signal(SIGCHLD, SIG_IGN);
    setenv("PATH", "bin:.", 1);

    PipeManager pipe_manager;

    while (true) {
        cout << "% ";
        string input;
        getline(cin, input);

        if (input.empty())
            continue;

        CommandParser parser(input, pipe_manager);
        parser.processCommands();
    }

    return 0;
}