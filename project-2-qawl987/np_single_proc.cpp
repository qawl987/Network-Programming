#include "npshell_single_proc.cpp"
#include <arpa/inet.h>
#define MAX_LINE 15000
#define MAXUSER 30
#define PROMPT "% "

const string WELCOME_MESSAGE = "****************************************\n"
                               "** Welcome to the information server. **\n"
                               "****************************************\n";

vector<UserInfo> userList(MAXUSER + 1);

unordered_map<pair<int, int>, pair<int, int>, pair_hash> userPipe;

int getUserIndex(int fd) {
    for (int idx = 1; idx <= MAXUSER; idx++) {
        if (userList[idx].fd == fd) {
            return idx;
        }
    }
    return -1;
}

void initUserInfos(int idx) {
    userList[idx].isLogin = false;
    userList[idx].id = 0;
    userList[idx].name = "(no name)";
    userList[idx].ipPort = "";
    userList[idx].fd = -1;
    userList[idx].env.clear();
    userList[idx].env["PATH"] = "bin:."; // initial PATH is bin/ and ./
    userList[idx].cmdCount = 0;
}

int shell(int fd) {
    char buf[10000];
    int n;
    memset(buf, 0, sizeof(buf));
    n = read(fd, buf, sizeof(buf));
    if (n == 0) {
        return -1;
    } else if (n < 0) {
        cerr << "echo read: " << strerror(errno) << endl;
        return -1; // Return -1 on error
    }

    string input(buf);
    input.erase(input.find_last_not_of(" \n\r\t") +
                1); // remove trailing whitespace
    // Redirect stdout, stderr
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);

    // Get the current user
    int userIndex = getUserIndex(fd);
    UserInfo *user = &userList.at(userIndex);

    // Clear the environment variables
    clearenv();
    // Set the environment variables for the user
    for (auto &env : user->env) {
        setenv(env.first.c_str(), env.second.c_str(), 1);
    }

    if (input.empty()) {
        cout << PROMPT << flush;
        return 0;
    } else if (input == "exit") {
        return -1;
    }

    CommandParser parser(input, user->pipeManager, user, userList, userPipe);
    parser.processCommands();
    cout << PROMPT << flush;
    return 0;
}

int createSocket(int port) {
    int listenfd;
    struct sockaddr_in serv_addr;
    // Create listening socket
    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("server: can't open stream socket");
        exit(1); // Exit on critical errors
    }

    // Prepare the sockaddr_in structure
    memset(&serv_addr, 0, sizeof(serv_addr)); // Use memset instead of bzero
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(port);

    // Allow address reuse - helpful for restarting server quickly
    int optval = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &optval,
                   sizeof(optval)) < 0) {
        perror("setsockopt(SO_REUSEADDR) failed");
    }

    // Bind the address
    if (bind(listenfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("server: can't bind local address");
        close(listenfd);
        exit(1);
    }

    // Listen for connections
    if (listen(listenfd, MAXUSER) < 0) { // Check listen return value
        perror("server: listen error");
        close(listenfd);
        exit(1);
    }
    return listenfd;
}

void deleteUserPipe(int id) {
    for (auto it = userPipe.begin(); it != userPipe.end();) {
        pair idPair = it->first;
        pair pipeFdPair = it->second;
        // (read, write)
        if (idPair.first == id || idPair.second == id) {
            close(pipeFdPair.first);
            close(pipeFdPair.second);
            it = userPipe.erase(it); // Erase the entry and update the iterator
        } else {
            ++it; // Move to the next entry
        }
    }
}

void userLogin(int msock, fd_set &afds) {
    struct sockaddr_in clientAddr;
    socklen_t alen = sizeof(clientAddr);
    int ssock = accept(msock, (struct sockaddr *)&clientAddr, &alen);
    if (ssock < 0) {
        cerr << "accept: " << strerror(errno) << endl;
    }
    FD_SET(ssock, &afds);

    char ipBuf[INET_ADDRSTRLEN];
    string ip = inet_ntoa(clientAddr.sin_addr);
    string clientIP =
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipBuf, sizeof(clientIP));
    string clientPort = to_string(ntohs(clientAddr.sin_port));
    write(ssock, WELCOME_MESSAGE.c_str(), WELCOME_MESSAGE.size());

    // Find the first available user slot
    for (int idx = 1; idx <= MAXUSER; idx++) {
        if (!userList[idx].isLogin) {
            PipeManager pipeManager;
            userList[idx].isLogin = true;
            userList[idx].id = idx;
            userList[idx].ipPort = clientIP + ":" + clientPort;
            userList[idx].fd = ssock;
            userList[idx].pipeManager = pipeManager;
            string msg = "*** User '" + userList[idx].name + "' entered from " +
                         userList[idx].ipPort + ". ***\n";
            ProcessExecutor::broadcastMessage(msg, userList);
            break;
        }
    }
    write(ssock, PROMPT, strlen(PROMPT));
}

void userLogout(int fd) {
    // Find the user index
    int userIndex = getUserIndex(fd);
    if (userIndex != -1) {
        userList[userIndex].isLogin =
            false; // logout (can't send message to the user who left)
        string msg = "*** User '" + userList[userIndex].name + "' left. ***\n";
        ProcessExecutor::broadcastMessage(msg, userList);
        initUserInfos(userIndex);
        deleteUserPipe(userIndex);
    }
}

int main(int argc, char *argv[]) {
    struct sockaddr_in clientAddr;
    int SERV_TCP_PORT = std::atoi(argv[1]);
    fd_set rfds, afds;
    socklen_t alen;
    int nfds;
    int msock = createSocket(SERV_TCP_PORT);
    nfds = FD_SETSIZE;
    FD_ZERO(&afds);
    FD_SET(msock, &afds);
    for (int i = 1; i <= MAXUSER; i++) {
        initUserInfos(i);
    }
    signal(SIGCHLD, SIG_IGN);
    while (1) {
        memcpy(&rfds, &afds, sizeof(rfds));
        if (select(nfds, &rfds, NULL, NULL, NULL) < 0) {
            if (errno != EINTR) {
                cerr << "Error in select, errno: " << errno << endl;
            }
            continue; // may be interrupted by signal or other errors -> select
                      // again
        }
        if (FD_ISSET(msock, &rfds)) {
            userLogin(msock, afds);
        }
        for (int fd = 0; fd < nfds; ++fd) {
            if (fd != msock && FD_ISSET(fd, &rfds)) {
                int status = shell(fd);
                if (status == -1) { // exit
                    userLogout(fd);
                    shutdown(fd, SHUT_RDWR); // close telnet
                    close(fd);
                    FD_CLR(fd, &afds);
                }
            }
        }
    }
}