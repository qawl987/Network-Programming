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
#define MAXUSER 30
using namespace std;

#define USER_PIPE_IN 0

// For unordered_map<pair<int, int>, pair<int, int>, pair_hash> userPipe;
struct pair_hash {
    size_t operator()(const pair<int, int> &p) const {
        return hash<int>()(p.first) ^ (hash<int>()(p.second) << 1);
    }
};

// No change in single_proc
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
    string name;
    string ipPort;
    int fd;
    unordered_map<string, string> env; // [var] [value] (e.g. [PATH] [bin:.])
    int cmdCount;                      // count the number of commands
    PipeManager pipeManager;           // store numbered pipe
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
    static void run(const ProcessConfig &config, UserInfo *user,
                    vector<UserInfo> &userList) {
        if (handleBuiltins(config, user, userList)) {
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

    static void broadcastMessage(const string &msg,
                                 vector<UserInfo> &userList) {
        for (int idx = 1; idx <= MAXUSER; idx++) {
            if (userList[idx].isLogin) {
                write(userList[idx].fd, msg.c_str(), msg.size());
            }
        }
    }

  private:
    // since run call these function, need static
    static bool handleBuiltins(const ProcessConfig &config, UserInfo *user,
                               vector<UserInfo> &userList) {
        const string &cmd = config.arguments[0];

        if (cmd == "exit") {
            exit(0);
        }

        if (cmd == "setenv") {
            user->env[config.arguments[1].c_str()] =
                config.arguments[2].c_str();
            setenv(config.arguments[1].c_str(), config.arguments[2].c_str(), 1);
            return true;
        }

        if (cmd == "printenv") {
            const char *env = getenv(config.arguments[1].c_str());
            if (env != NULL) {
                string msg = env + string("\n");
                write(user->fd, msg.c_str(), msg.size());
            }
            return true;
        }

        if (cmd == "who") {
            string msg = "<ID>\t<nickname>\t<IP:port>\t<indicate me>\n";
            for (int idx = 1; idx <= MAXUSER; idx++) {
                if (userList[idx].isLogin) {
                    msg += to_string(userList[idx].id) + "\t" +
                           userList[idx].name + "\t" + userList[idx].ipPort;
                    if (idx == user->id) {
                        msg += "\t<-me";
                    }
                    msg += "\n";
                }
            }
            write(user->fd, msg.c_str(), msg.size());
            return true;
        }

        if (cmd == "tell") {
            int targetId = stoi(config.arguments[1]);
            string msg = "";
            if (!userList[targetId].isLogin) {
                msg += "*** Error: user #" + to_string(targetId) +
                       " does not exist yet. ***\n";
                write(user->fd, msg.c_str(), msg.size());
            } else { // send message to target user
                msg += "*** " + user->name + " told you ***: ";
                for (int i = 2; i < config.arguments.size(); i++) {
                    msg += config.arguments[i];
                    if (i != config.arguments.size() - 1) {
                        msg += " ";
                    }
                }
                msg += "\n";
                write(userList[targetId].fd, msg.c_str(), msg.size());
            }
            return true;
        }

        if (cmd == "yell") {
            string msg = "*** " + user->name + " yelled ***: ";
            for (int i = 1; i < config.arguments.size(); i++) {
                msg += config.arguments[i];
                if (i != config.arguments.size() - 1) {
                    msg += " ";
                }
            }
            msg += "\n";
            broadcastMessage(msg, userList);
            return true;
        }

        if (cmd == "name") {
            bool isNameExist = false;
            for (int idx = 1; idx <= MAXUSER; idx++) {
                if (userList[idx].isLogin &&
                    userList[idx].name == config.arguments[1]) {
                    cout << "*** User '" << config.arguments[1]
                         << "' already exists. ***" << endl;
                    isNameExist = true;
                }
            }
            if (!isNameExist) {
                user->name = config.arguments[1];
                string msg = "*** User from " + user->ipPort + " is named '" +
                             user->name + "'. ***\n";
                broadcastMessage(msg, userList);
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

class CommandParser {
    stringstream input_stream;
    vector<string> current_args;
    PipeManager &pipe_manager;
    UserInfo *userInfo;
    vector<UserInfo> &userList;
    unordered_map<pair<int, int>, pair<int, int>, pair_hash> &userPipe;
    string lineCommand;
    string pipeOutMsg;

  public:
    CommandParser(
        const string &line, PipeManager &pm, UserInfo *userInfo,
        vector<UserInfo> &userList,
        unordered_map<pair<int, int>, pair<int, int>, pair_hash> &userPipe)
        : input_stream(line), pipe_manager(pm), userInfo(userInfo),
          userList(userList), userPipe(userPipe), lineCommand(line) {}

    void processCommands() {
        string token;

        while (input_stream >> token) {
            // If is tell or yell, put all in arg and break
            if (isBuiltinToken(token)) {
                current_args.push_back(token);
                // actually lineCommand do the thing below, no need anymore
                while (input_stream >> token)
                    current_args.push_back(token);
                break;
            }
            if (isOperator(token[0])) {
                executeCurrentCommand(token);
            } else {
                current_args.push_back(token);
            }
        }

        executeRemainingCommand();
    }

  private:
    bool isBuiltinToken(string s) const { return s == "yell" || s == "tell"; }

    bool isOperator(char c) const {
        return c == '|' || c == '!' || c == '>' || c == '<';
    }

    void executeCurrentCommand(string &operator_token) {
        ProcessExecutor::ProcessConfig config;
        config.arguments = current_args;
        current_args.clear();
        setupInputPipe(config);
        // because testcase cat <2 >3, cat <3 >2. So require 1-token lookahead
        // here to set up pipe before run.
        handleOperator(operator_token, config);

        ProcessExecutor::run(config, userInfo, userList);
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
            } else if (op.length() == 2) {
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
        ProcessExecutor::broadcastMessage(pipeOutMsg, userList);
    }
    void handleRedirection(ProcessExecutor::ProcessConfig &config) {
        string filename;
        input_stream >> filename;
        config.output_fd = open(filename.c_str(),
                                O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0664);
    }

    void handleOutUserPipe(const string &op,
                           ProcessExecutor::ProcessConfig &config) {
        int sendUserId = userInfo->id;
        int recvUserId = stoi(op.substr(1));
        // recv user not exist
        if (recvUserId < 0 || recvUserId > 30 ||
            !userList[recvUserId].isLogin) {
            config.userPipeToErr = true;
            string msg = "*** Error: user #" + to_string(recvUserId) +
                         " does not exist yet. ***\n";
            setupInputPipe(config); // trigger userPipeToErr again
            write(userInfo->fd, msg.c_str(), msg.size());
            return;
        }
        auto it = userPipe.find(make_pair(recvUserId, sendUserId));
        if (it != userPipe.end()) {
            // found it
            config.userPipeToErr = true;
            string msg = "*** Error: the pipe #" + to_string(sendUserId) +
                         "->#" + to_string(recvUserId) +
                         " already exists. ***\n";
            setupInputPipe(config); // trigger userPipeToErr again
            write(userInfo->fd, msg.c_str(), msg.size());
        } else {
            int pipe_fds[2];
            while (pipe(pipe_fds) == -1) {
                if (errno == EMFILE || errno == ENFILE) {
                    wait(nullptr);
                }
            }
            userPipe[make_pair(recvUserId, sendUserId)] =
                make_pair(pipe_fds[0], pipe_fds[1]);
            fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC);
            fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC);
            config.output_fd = pipe_fds[1];
            config.error_fd = pipe_fds[1];
            string msg = "*** " + userInfo->name + " (#" +
                         to_string(userInfo->id) + ") just piped '" +
                         lineCommand + "' to " + userList[recvUserId].name +
                         " (#" + to_string(recvUserId) + ") ***\n";
            // save message for last output. Ex: cat >3 <2
            pipeOutMsg = msg;
        }
    }

    void handleInUserPipe(const string &op,
                          ProcessExecutor::ProcessConfig &config) {
        int sendUserId = stoi(op.substr(1));
        // sender not exist
        if (sendUserId < 0 || sendUserId > 30 ||
            !userList[sendUserId].isLogin) { // the source user does not exist
            config.userPipeFromErr = true;
            string msg = "*** Error: user #" + to_string(sendUserId) +
                         " does not exist yet. ***\n";
            setupInputPipe(config); // trigger userPipeFromErr again
            write(userInfo->fd, msg.c_str(), msg.size());
            return;
        }
        auto it = userPipe.find(make_pair(userInfo->id, sendUserId));
        if (it != userPipe.end()) {
            // Found it: have pipe to read
            pair pipeFd = it->second;
            int inputFd = pipeFd.first;
            int outFd = pipeFd.second;
            config.pipe[0] = inputFd;
            config.pipe[1] = outFd;
            // remove userPipe map after read pipe
            it = userPipe.erase(it);
            string msg = "*** " + userList[userInfo->id].name + " (#" +
                         to_string(userInfo->id) + ") just received from " +
                         userList[sendUserId].name + " (#" +
                         to_string(sendUserId) + ") by '" + lineCommand +
                         "' ***\n";
            // directly broadcast, since "<" broadcast before ">"
            ProcessExecutor::broadcastMessage(msg, userList);
        } else {
            // Not found: pipe doesn't create
            config.userPipeFromErr = true;
            string msg = "*** Error: the pipe #" + to_string(sendUserId) +
                         "->#" + to_string(userInfo->id) +
                         " does not exist yet. ***\n";
            setupInputPipe(config); // trigger userPipeFromErr again
            write(userInfo->fd, msg.c_str(), msg.size());
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

    void executeRemainingCommand() {
        if (!current_args.empty()) {
            ProcessExecutor::ProcessConfig config;
            config.arguments = current_args;
            setupInputPipe(config);

            ProcessExecutor::run(config, userInfo, userList);
            // Command in here is definitely one command(ex: number,
            // removetag, impossible |n & !n)
            pipe_manager.shiftPipeNumbers();
        }
    }
};
