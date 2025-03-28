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
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
using namespace std;

void sigFork(int signo) {
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) {
    }
}

struct Command {
    vector<string> args;
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
        // wait is important
        waitpid(child, nullptr, 0);
        return;
    }

    // child process
    vector<char *> args;
    for (const auto &arg : command.args) {
        args.push_back(strdup(arg.c_str()));
    }
    args.push_back(nullptr);
    if (execvp(args[0], args.data()) == -1 && errno == ENOENT) {
        cerr << "Unknown command: [" << args[0] << "].\n";
        exit(0);
    }
    return;
}

void initShell() {
    clearenv();
    setenv("PATH", "bin:.", 1);
    signal(SIGCHLD, sigFork); // call wait() catch state
}

vector<Command> parseCommands(string input) {
    vector<Command> commands;
    vector<string> command_args;
    stringstream ss(input);
    string arg;
    while (getline(ss, arg, ' ')) {
        if (arg[0] == '|' || arg[0] == '!' || arg[0] == '>') {
            Command command;
            command.args = command_args;
            command_args.clear();
            commands.push_back(command);
        } else {
            command_args.push_back(arg);
        }
    }
    if (!command_args.empty()) { // parse last command
        Command command;
        command.args = command_args;
        commands.push_back(command);
    }
    return commands;
}

int main() {
    initShell();
    while (true) {
        string input;
        cout << "% ";
        getline(cin, input);
        if (input.empty()) {
            continue;
        }
        vector<Command> commands = parseCommands(input);
        for (auto &command : commands) {
            executeCommand(command);
        }
    }
    return 0;
}