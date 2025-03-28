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
#include <vector>
using namespace std;

void sigFork(int signo) {
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) {
    }
}

struct Command {
    vector<string> args;
    int fd_out = STDOUT_FILENO;
    int fd_err = STDERR_FILENO;
    // 0: fd_in, 1: fd write to fd_in
    int pipe[2] = {STDIN_FILENO, STDOUT_FILENO};
};

void executeCommand(const Command &command) {
    if (command.args[0] == "exit") {
        exit(0);
    }

    if (command.args[0] == "setenv") {
        setenv(command.args[1].c_str(), command.args[2].c_str(), 1);
        return;
    }

    if (command.args[0] == "printenv") {
        if (const char *env = getenv(command.args[1].c_str())) {
            cout << env << '\n';
        }
        return;
    }

    pid_t child;
    while ((child = fork()) == -1) {
        if (errno == EAGAIN) {
            wait(nullptr); // wait for any child process to release resource
        }
    }
    // parent process
    if (child != 0) {
        // close pipe
        if (command.pipe[0] != STDIN_FILENO) {
            close(command.pipe[0]);
            close(command.pipe[1]);
        }
        // wait is important
        struct stat fd_stat;
        fstat(command.fd_out, &fd_stat);
        // close file if fd_out isn't STDOUT_FILENO
        if (command.fd_out != STDOUT_FILENO && S_ISREG(fd_stat.st_mode)) {
            close(command.fd_out);
        }
        // wait for child when fd_out isn't pipe
        if (!S_ISFIFO(fd_stat.st_mode)) {
            waitpid(child, nullptr, 0);
        }
        return;
    }

    // child process
    dup2(command.pipe[0], STDIN_FILENO);
    dup2(command.fd_out, STDOUT_FILENO);
    dup2(command.fd_err, STDERR_FILENO);
    // Close the original pipe file descriptors
    if (command.pipe[0] != STDIN_FILENO) {
        close(command.pipe[0]);
    }
    if (command.pipe[1] != STDOUT_FILENO) {
        close(command.pipe[1]);
    }
    vector<char *> args;
    for (const auto &arg : command.args) {
        args.push_back(strdup(arg.c_str()));
    }
    args.push_back(nullptr);
    if (execvp(args[0], args.data()) == -1 && errno == ENOENT) {
        cerr << "Unknown command: [" << args[0] << "].\n";
        // Free allocated memory before exiting
        for (char *arg : args) {
            free(arg);
        }
        exit(0);
    }
    return;
}

void initShell() {
    clearenv();
    setenv("PATH", "bin:.", 1);
    signal(SIGCHLD, sigFork); // call wait() catch state
}

void update_pipeMap(unordered_map<int, pair<int, int>> &pipeMap) {
    unordered_map<int, pair<int, int>> new_map;
    for (const auto [key, value] : pipeMap) {
        new_map.emplace(key - 1, value); // reduce pipeNum
    }
    pipeMap = new_map;
}

vector<Command> parseCommands(string input,
                              unordered_map<int, pair<int, int>> &pipeMap) {
    vector<Command> commands;
    vector<string> command_args;
    stringstream ss(input);
    string arg;
    while (getline(ss, arg, ' ')) {
        if (arg[0] == '|' || arg[0] == '!' || arg[0] == '>') {
            Command command;
            command.args = command_args;
            command_args.clear();

            if (pipeMap.count(0)) { // pipe fd_in
                command.pipe[0] = pipeMap[0].first;
                command.pipe[1] = pipeMap[0].second;
                pipeMap.erase(0);
            }

            if (arg[0] == '>') {
                string filename;
                getline(ss, filename, ' ');
                command.fd_out =
                    open(filename.c_str(),
                         O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0664);
            } else {
                int pipeNum = 0;
                if (arg.size() > 1) {
                    pipeNum = stoi(arg.substr(1));
                }

                if (pipeMap.count(pipeNum) == 0) {
                    int pipe_fd[2];
                    while (pipe(pipe_fd) == -1) {
                        if (errno == EMFILE || errno == ENFILE) {
                            wait(nullptr);
                        }
                    }
                    pipeMap.emplace(pipeNum, make_pair(pipe_fd[0], pipe_fd[1]));
                }
                if (arg[0] == '|') {
                    command.fd_out = pipeMap[pipeNum].second;
                }

                if (arg[0] == '!') { // pipe fd_out, fd_err
                    command.fd_out = pipeMap[pipeNum].second;
                    command.fd_err = pipeMap[pipeNum].second;
                }
            }
            commands.push_back(command);
            if (arg != "|") { // "|" piping without updating pipeMap
                update_pipeMap(pipeMap);
            }
        } else {
            command_args.push_back(arg);
        }
    }
    if (!command_args.empty()) { // parse last command
        Command command;
        command.args = command_args;

        if (pipeMap.count(0)) { // pipe fd_in
            command.pipe[0] = pipeMap[0].first;
            command.pipe[1] = pipeMap[0].second;
            pipeMap.erase(0);
        }
        commands.push_back(command);
        update_pipeMap(pipeMap);
    }
    return commands;
}

int main() {
    initShell();

    unordered_map<int, pair<int, int>> pipeMap; // 0: read, 1: write
    while (true) {
        string input;
        cout << "% ";
        getline(cin, input);
        if (input.empty()) {
            continue;
        }
        vector<Command> commands = parseCommands(input, pipeMap);
        for (auto &command : commands) {
            executeCommand(command);
        }
    }
    return 0;
}