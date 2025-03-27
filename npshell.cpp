#include <ctype.h>
#include <fcntl.h>
#include <iostream>
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
    while (waitpid(-1, &status, WNOHANG)) {
    }
}

struct Command {
    vector<string> args;
};

void parse(Command &c, string input) {
    stringstream ss(input);
    string arg;
    while (getline(ss, arg, ' ')) {
        cout << arg << endl;
        if (arg[0] == '|') {
            cout << "pipe" << endl;
        } else if (arg[0] == '!') {
            cout << "error" << endl;
        } else if (arg[0] == '>') {
            cout << "redirect" << endl;
        } else {
            cout << "command" << endl;
            c.args.push_back(arg);
        }
    }
    return;
}

void execute(Command &c) {
    pid_t child;
    while ((child = fork()) == -1) {
        if (errno == EAGAIN) {
            wait(nullptr); // wait for any child process to release resource
        }
    }

    if (child != 0) { // parent process
        // close pipe
        return;
    }
    // auto args = make_unique<char *[]>(command.args.size() + 1);
    // for (size_t i = 0; i < command.args.size(); i++) {
    //     args[i] = strdup(command.args[i].c_str());
    // }
    // args[command.args.size()] = nullptr;

    // if (execvp(args[0], args.get()) == -1 && errno == ENOENT) {
    //     cerr << "Unknown command: [" << args[0] << "].\n";
    //     exit(0);
    // }
    // for (auto &bin : c.binGroup) {
    //     cout << bin.first << " " << bin.second << endl;
    // }
}

void initShell() {
    clearenv();
    setenv("PATH", "bin:.", 1);
    signal(SIGCHLD, sigFork); // call wait() catch state
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
        Command c;
        // If
        vector<Command> commandList;
        parse(c, input);
        execute(c);
        // for (auto &command : c.commands) {
        //     cout << command << endl;
        // }
        // for (auto &arg : c.args) {
        //     cout << arg << endl;
        // }
    }
}