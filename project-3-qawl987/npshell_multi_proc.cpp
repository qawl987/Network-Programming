#include <ctype.h>
#include <fcntl.h>
#include <array>
#include <iostream>
#include <map>
#include <memory>
#include <queue>
#include <semaphore.h>
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
#define MAX_LINE 15000
#define MAXUSER 30
#define PERMS 0666

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

struct UserInfo {
    bool isLogin; // check if the user is login
    int id;       // range from 1 to 30
    char name[25];
    char ipPort[25];
    // Master server write to here = ms_pipe[1]. Slave server read from ms_pipe[0]
    int ms_pipe; 
};

// Utility class, instance independent
class ProcessExecutor {
  public:
    struct ProcessConfig {
        vector<string> arguments;
        int pipe[2] = {STDIN_FILENO, STDOUT_FILENO};
        int output_fd = STDOUT_FILENO;
        int error_fd = STDERR_FILENO;
        bool userPipeFromErr = false;
        bool userPipeToErr = false;
    };
    // static method bind on class
    static bool run(const ProcessConfig &config, int user_id, sem_t *read_lock,
                    sem_t *write_lock, array<int, 2> shared_pipe,
                    UserInfo *userList) {
        bool need_bash = true;
        if (handleBuiltins(config, user_id, read_lock, write_lock, shared_pipe,
                           userList, need_bash)) {
            return need_bash;
        }

        pid_t pid = createChildProcess();
        if (pid != 0) {
            cleanupParentResources(config);
            waitForChildIfNeeded(pid, config);
            return true;
        }

        setupChildProcessIO(config);
        // In fact, child and parent choose one close the originally fd before
        // dup2 is fine, but close both side in case
        removeNonNecessaryPipes(config);
        executeExternalCommand(config);
        return true;
    }

    static void broadcastMessage(string msg, int user_id, sem_t *read_lock,
                                 sem_t *write_lock, array<int, 2> shared_pipe) {
        sem_wait(write_lock);
        write(shared_pipe[1], &user_id, sizeof(user_id));
        size_t size = msg.size();
        write(shared_pipe[1], &size, sizeof(size));
        write(shared_pipe[1], msg.c_str(), size);
        sem_post(read_lock);
    }

  private:
    // since run call these function, need static
    static bool handleBuiltins(const ProcessConfig &config, int user_id,
                               sem_t *read_lock, sem_t *write_lock,
                               array<int, 2> shared_pipe, UserInfo *userList,
                               bool &need_bash) {
        const string &cmd = config.arguments[0];
        UserInfo *user = &userList[user_id];
        if (cmd == "exit") {
            string msg =
                "exit *** User \'" + string(user->name) + "\' left. ***";
            ProcessExecutor::broadcastMessage(msg, user_id, read_lock,
                                              write_lock, shared_pipe);
            // exit(0);
            need_bash = false;
            return true;
        }

        if (cmd == "setenv") {
            setenv(config.arguments[1].c_str(), config.arguments[2].c_str(), 1);
            need_bash = true;
            return true;
        }

        if (cmd == "printenv") {
            if (const char *env = getenv(config.arguments[1].c_str())) {
                cout << env << '\n';
            }
            need_bash = true;
            return true;
        }

        if (cmd == "who") {
            string msg = "<ID>\t<nickname>\t<IP:port>\t<indicate me>\n";
            for (int idx = 1; idx <= MAXUSER; idx++) {
                if (userList[idx].isLogin) {
                    msg += to_string(userList[idx].id) + "\t" +
                           userList[idx].name + "\t" + userList[idx].ipPort;
                    if (idx == user_id) {
                        msg += "\t<-me";
                    }
                    msg += "\n";
                }
            }
            cout << msg.c_str();
            need_bash = true;
            return true;
        }

        if (cmd == "tell") {
            int recevier_id = stoi(config.arguments[1]);
            string msg = "";
            if (!userList[recevier_id].isLogin) {
                msg += "*** Error: user #" + to_string(recevier_id) +
                       " does not exist yet. ***\n";
                need_bash = true;
                cout << msg << flush;
            } else { // send message to target user
                sem_wait(write_lock);
                write(shared_pipe[1], &user_id, sizeof(user_id));
                msg = "tell " + config.arguments[1] + " *** " + user->name +
                      " told you ***: ";
                for (int i = 2; i < config.arguments.size(); i++) {
                    msg += config.arguments[i];
                    if (i != config.arguments.size() - 1) {
                        msg += " ";
                    }
                }
                msg += "\n";
                size_t size = msg.size();
                write(shared_pipe[1], &size, sizeof(size));
                write(shared_pipe[1], msg.c_str(), size);
                sem_post(read_lock);
                need_bash = false;
            }
            return true;
        }

        if (cmd == "yell") {
            string msg = "yell *** " + string(user->name) + " yelled ***: ";
            for (int i = 1; i < config.arguments.size(); i++) {
                msg += config.arguments[i];
                if (i != config.arguments.size() - 1) {
                    msg += " ";
                }
            }
            msg += "\n";
            ProcessExecutor::broadcastMessage(msg, user_id, read_lock,
                                              write_lock, shared_pipe);
            need_bash = false;
            return true;
        }

        if (cmd == "name") {
            for (int idx = 1; idx <= MAXUSER; idx++) {
                if (userList[idx].isLogin &&
                    userList[idx].name == config.arguments[1]) {
                    cout << "*** User '" << config.arguments[1]
                         << "' already exists. ***" << endl;
                    need_bash = true;
                    return true;
                }
            }
            strcpy(user->name, config.arguments[1].c_str());
            string msg = "name " + string("*** User from ") +
                         string(user->ipPort) + " is named \'" +
                         config.arguments[1] + "\'. ***";
            ProcessExecutor::broadcastMessage(msg, user_id, read_lock,
                                              write_lock, shared_pipe);
            need_bash = false;
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

class CommandParser {
    stringstream input_stream;
    vector<string> current_args;
    PipeManager &pipe_manager;
    sem_t *read_lock;
    sem_t *write_lock;
    array<int, 2> shared_pipe;
    int user_id;
    UserInfo *userList;
    map<int, pair<int, int>> &user_pipe_fds;
    string user_pipe_msg;
    string line_command;

  public:
    CommandParser(const string &line, PipeManager &pm, int user_id,
                  sem_t *read_lock, sem_t *write_lock,
                  array<int, 2> shared_pipe, UserInfo *userList,
                  map<int, pair<int, int>> &user_pipe_fds)
        : input_stream(line), pipe_manager(pm), user_id(user_id),
          read_lock(read_lock), write_lock(write_lock),
          shared_pipe(shared_pipe), userList(userList),
          user_pipe_fds(user_pipe_fds), line_command(line) {}

    bool processCommands() {
        string token;
        bool need_bash = true;
        while (input_stream >> token) {
            if (isBuiltinToken(token)) {
                current_args.push_back(token);
                // actually lineCommand do the thing below, no need anymore
                while (input_stream >> token)
                    current_args.push_back(token);
                break;
            }
            if (isOperator(token[0])) {
                need_bash = executeCurrentCommand(token);
            } else {
                current_args.push_back(token);
            }
        }

        need_bash = executeRemainingCommand();
        return need_bash;
    }

  private:
    bool isBuiltinToken(string s) const { return s == "yell" || s == "tell"; }
    bool isOperator(char c) const {
        return c == '|' || c == '!' || c == '>' || c == '<';
    }

    bool executeCurrentCommand(string &operator_token) {
        ProcessExecutor::ProcessConfig config;
        config.arguments = current_args;
        current_args.clear();

        setupInputPipe(config);
        handleOperator(operator_token, config);

        if (!user_pipe_msg.empty()) {
            cout << user_pipe_msg << flush;
            user_pipe_msg.insert(0, "user_pipe "s);
            ProcessExecutor::broadcastMessage(user_pipe_msg, user_id, read_lock,
                                              write_lock, shared_pipe);
        }

        bool need_bash = ProcessExecutor::run(
            config, user_id, read_lock, write_lock, shared_pipe, userList);
        // Here is mid of command, so only shift when "|n" or "!n". ">" doesn't
        // enter here
        if (operator_token != "|") {
            pipe_manager.shiftPipeNumbers();
        }
        return need_bash;
    }

    void setupInputPipe(ProcessExecutor::ProcessConfig &config) {
        // still need assign pipe[1] for fd_in process close fd
        if (pipe_manager.hasPipe(0)) {
            int *pipe_fds = pipe_manager.active_pipes[0];
            config.pipe[0] = pipe_fds[0];
            config.pipe[1] = pipe_fds[1];
            pipe_manager.removePipe(0);
        }
        // If following command have error, redirect output to null
        if (config.userPipeFromErr) {
            config.pipe[0] = open("/dev/null", O_RDWR);
            config.pipe[1] = open("/dev/null", O_RDWR);
        }
        if (config.userPipeToErr) {
            config.output_fd = open("/dev/null", O_RDWR);
            config.pipe[0] = open("/dev/null", O_RDWR);
            config.pipe[1] = open("/dev/null", O_RDWR);
        }
    }

    void handleOperator(string &op, ProcessExecutor::ProcessConfig &config) {
        user_pipe_msg = "";
        if (op[0] == '<') {
            handleInUserPipe(op, config);
            // cat <3 | cat, cat <3 >2. 1-token lookahead.
            if (!(input_stream >> op)) { // get next arg for output
                op.clear();
            }
        }
        if (op[0] == '>') {
            if (op.length() == 1) {
                handleRedirection(config);
            } else if (op.length() >= 2) {
                handleOutUserPipe(op, config);
            }
        } else {
            handlePiping(op, config);
        }
        // pipe from user_pipe if possible
        // cat >3 <2. 1-token lookahead.
        string opBackup = op;
        // peek
        streampos position = input_stream.tellg();
        if ((input_stream >> op) && op[0] == '<') {
            handleInUserPipe(op, config);
        } else {
            // reset op. Ex: "|" can't change for
            // pipe_manager.shiftPipeNumbers();
            input_stream.seekg(position);
            op = opBackup;
        }
    }

    void handleRedirection(ProcessExecutor::ProcessConfig &config) {
        string filename;
        input_stream >> filename;
        config.output_fd = open(filename.c_str(),
                                O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0664);
    }

    void handleOutUserPipe(const string &op,
                           ProcessExecutor::ProcessConfig &config) {
        int sender_id = user_id;
        int receiver_id = stoi(op.substr(1));
        UserInfo *user = &userList[sender_id];
        // recv user not exist
        if (receiver_id < 0 || receiver_id > 30 ||
            !userList[receiver_id].isLogin) {
            config.userPipeToErr = true;
            string msg = "*** Error: user #" + to_string(receiver_id) +
                         " does not exist yet. ***\n";
            setupInputPipe(config); // trigger userPipeToErr again
            cout << msg << flush;
            return;
        }
        string user_pipe_filename =
            "user_pipe/" + to_string(receiver_id) + "_" + to_string(sender_id);
        if (mkfifo(user_pipe_filename.c_str(), PERMS) <
            0) { // user pipe already exists
            config.userPipeToErr = true;
            string msg = "*** Error: the pipe #" + to_string(sender_id) +
                         "->#" + to_string(receiver_id) +
                         " already exists. ***\n";
            setupInputPipe(config);
            cout << msg << flush;
        } else {
            string msg = "> " + to_string(receiver_id);
            ProcessExecutor::broadcastMessage(msg, user_id, read_lock,
                                              write_lock, shared_pipe);
            int output_fd = open(user_pipe_filename.c_str(), O_WRONLY);
            if (output_fd < 0) {
                cout << "Error: failed to open writefd for user pipe" << endl
                     << flush;
            }
            // no need for err_fd redirect, won't occur in user pipe
            config.output_fd = output_fd;
            // map<int, pair<int, int>> would auto insert (0, output_fd), or update (exist, output_fd)
            user_pipe_fds[receiver_id].second = output_fd;
            // Put pipe_fd to pipe[1], for removeNonNecessaryPipes to close
            msg = "*** " + string(user->name) + " (#" + to_string(user_id) +
                  ") just piped \'" + line_command + "\' to " +
                  userList[receiver_id].name + " (#" + to_string(receiver_id) +
                  ") ***\n";
            user_pipe_msg.append(msg);
        }
    }

    void handleInUserPipe(const string &op,
                          ProcessExecutor::ProcessConfig &config) {
        int sender_id = stoi(op.substr(1));
        int receiver_id = user_id;
        UserInfo *user = &userList[user_id];
        // sender not exist
        if (sender_id < 0 || sender_id > 30 ||
            !userList[sender_id].isLogin) { // the source user does not exist
            config.userPipeFromErr = true;
            string msg = "*** Error: user #" + to_string(sender_id) +
                         " does not exist yet. ***\n";
            setupInputPipe(config); // trigger userPipeFromErr again
            cout << msg << flush;
            return;
        }
        if (user_pipe_fds.count(sender_id) &&
            user_pipe_fds[sender_id].first != 0) {
            config.pipe[0] = user_pipe_fds[sender_id].first;
            user_pipe_fds[sender_id].first = 0;
            string user_pipe_filename = "user_pipe/" + to_string(receiver_id) +
                                        "_" + to_string(sender_id);
            if (unlink(user_pipe_filename.c_str()) < 0) {
                cerr << "Error: failed to unlink user pipe" << endl;
            }
            string msg = "< " + to_string(sender_id);
            ProcessExecutor::broadcastMessage(msg, user_id, read_lock,
                                              write_lock, shared_pipe);
            msg = "*** " + string(user->name) + " (#" + to_string(receiver_id) +
                  ") just received from " + string(userList[sender_id].name) +
                  " (#" + to_string(sender_id) + ") by \'" + line_command +
                  "\' ***\n";
            if (user_pipe_msg.empty()) {
                user_pipe_msg.append(msg);
            } else {
                user_pipe_msg.insert(0, msg);
            }
        } else {
            config.userPipeFromErr = true;
            string msg = "*** Error: the pipe #" + to_string(sender_id) +
                         "->#" + to_string(receiver_id) +
                         " does not exist yet. ***\n";
            setupInputPipe(config); // trigger userPipeFromErr again
            cout << msg << flush;
        }
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

    bool executeRemainingCommand() {
        bool need_bash = true;
        if (!current_args.empty()) {
            ProcessExecutor::ProcessConfig config;
            config.arguments = current_args;
            setupInputPipe(config);

            need_bash = ProcessExecutor::run(config, user_id, read_lock,
                                             write_lock, shared_pipe, userList);
            // Command in here is definitely one command(ex: number, removetag,
            // impossible |n & !n)
            pipe_manager.shiftPipeNumbers();
        }
        return need_bash;
    }
};