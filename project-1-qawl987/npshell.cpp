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

// Utility class, instance independent
class ProcessExecutor {
  public:
    struct ProcessConfig {
        vector<string> arguments;
        int pipe[2] = {STDIN_FILENO, STDOUT_FILENO};
        int output_fd = STDOUT_FILENO;
        int error_fd = STDERR_FILENO;
    };
    // static method bind on class
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
        // In fact, child and parent choose one close the originally fd before
        // dup2 is fine, but close both side in case
        removeNonNecessaryPipes(config);
        executeExternalCommand(config);
    }

  private:
    // since run call these function, need static
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
        // close pipe when this command is the last command to use pipe
        // i.e., cat test.html | number (cat's pipe[0] = 0,number's pipe[0]=3)
        removeNonNecessaryPipes(config);

        struct stat fd_stat;
        // get the fd file type
        fstat(config.output_fd, &fd_stat);
        // close file if fd_out isn't STDOUT_FILENO and is a regular file.
        if (config.output_fd != STDOUT_FILENO && S_ISREG(fd_stat.st_mode)) {
            close(config.output_fd);
        }
    }

    static void waitForChildIfNeeded(pid_t pid, const ProcessConfig &config) {
        struct stat fd_stat;
        fstat(config.output_fd, &fd_stat);
        // wait for child when fd_out isn't pipe, i.e., is a regular file or
        // std_out.
        // EX1: ls. shell need wait ls output to process next command
        // EX2: large_file_process | cat. Pipe buffer have limit, so don't wait,
        // 'cat' can consume the pipe without deadlock problem
        // EX3: ls | cat, parent won't wait ls(output_fd is pipe), but wait
        // cat(output_fd is std_out)
        if (!S_ISFIFO(fd_stat.st_mode)) {
            waitpid(pid, nullptr, 0);
        }
    }

    static void setupChildProcessIO(const ProcessConfig &config) {
        // Set fd_in from SetupInputPipe
        dup2(config.pipe[0], STDIN_FILENO);
        // Set fd_out and fd_err from HandlePiping, if no specified, use
        // STDOUT_FILENO and STDERR_FILENO
        dup2(config.output_fd, STDOUT_FILENO);
        dup2(config.error_fd, STDERR_FILENO);
    }

    static void removeNonNecessaryPipes(const ProcessConfig &config) {
        // Close the original pipe file descriptors
        if (config.pipe[0] != STDIN_FILENO) {
            close(config.pipe[0]);
        }
        if (config.pipe[1] != STDOUT_FILENO) {
            close(config.pipe[1]);
        }
    }

    static void executeExternalCommand(const ProcessConfig &config) {
        auto argv = prepareCommandArguments(config);
        if (execvp(argv[0], argv.data()) == -1 && errno == ENOENT) {
            cerr << "Unknown command: [" << argv[0] << "].\n";
            exit(0);
        }
    }

    static vector<char *> prepareCommandArguments(const ProcessConfig &config) {
        vector<char *> args;
        for (const auto &arg : config.arguments) {
            args.push_back(strdup(arg.c_str()));
        }
        args.push_back(nullptr);
        return args;
    }
};

class PipeManager {

  public:
    unordered_map<int, int[2]> active_pipes;
    void createPipe(int pipe_id) {
        int pipe_fds[2];
        while (pipe(pipe_fds) == -1) {
            if (errno == EMFILE || errno == ENFILE) {
                wait(nullptr);
            }
        }
        active_pipes[pipe_id][0] = pipe_fds[0];
        active_pipes[pipe_id][1] = pipe_fds[1];
    }

    bool hasPipe(int pipe_id) const { return active_pipes.count(pipe_id) > 0; }

    void removePipe(int pipe_id) { active_pipes.erase(pipe_id); }

    void shiftPipeNumbers() {
        unordered_map<int, int[2]> new_pipes;
        for (const auto &[id, fds] : active_pipes) {
            new_pipes[id - 1][0] = fds[0];
            new_pipes[id - 1][1] = fds[1];
        }
        active_pipes = new_pipes;
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
    bool isOperator(char c) const {
        return c == '|' || c == '!' || c == '>' || c == '<';
    }

    void executeCurrentCommand(const string &operator_token) {
        ProcessExecutor::ProcessConfig config;
        config.arguments = current_args;
        current_args.clear();

        setupInputPipe(config);
        handleOperator(operator_token, config);

        ProcessExecutor::run(config);
        // Here is mid of command, so only shift when "|n" or "!n". ">" doesn't
        // enter here
        if (operator_token != "|") {
            pipe_manager.shiftPipeNumbers();
        }
    }

    void setupInputPipe(ProcessExecutor::ProcessConfig &config) {
        // still need assign pipe[1] for fd_in process close fd
        if (pipe_manager.hasPipe(0)) {
            int *pipe_fds = pipe_manager.active_pipes[0];
            config.pipe[0] = pipe_fds[0];
            config.pipe[1] = pipe_fds[1];
            pipe_manager.removePipe(0);
        }
    }

    void handleOperator(const string &op,
                        ProcessExecutor::ProcessConfig &config) {
        if (op[0] == '>') {
            if (op.size() > 1) {
                handleAppendRedirection(config);
            } else {
                handleRedirection(config);
            }
        } else if (op[0] == '<') {
            handleInputRedirection(config);
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
    void handleAppendRedirection(ProcessExecutor::ProcessConfig &config) {
        string filename;
        input_stream >> filename;
        config.output_fd = open(
            filename.c_str(), O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0664);
    }

    void handleInputRedirection(ProcessExecutor::ProcessConfig &config) {
        string filename;
        input_stream >> filename;
        config.pipe[0] = open(filename.c_str(), O_RDONLY | O_CLOEXEC);
    }

    void handlePiping(const string &op,
                      ProcessExecutor::ProcessConfig &config) {
        int pipe_id = op.size() > 1 ? stoi(op.substr(1)) : 0;

        if (!pipe_manager.hasPipe(pipe_id)) {
            pipe_manager.createPipe(pipe_id);
        }

        int *pipe_fds = pipe_manager.active_pipes[pipe_id];

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
            config.arguments = current_args;
            setupInputPipe(config);

            ProcessExecutor::run(config);
            // Command in here is definitely one command(ex: number, removetag,
            // impossible |n & !n)
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