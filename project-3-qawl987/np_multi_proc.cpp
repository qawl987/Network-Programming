#include "npshell_multi_proc.cpp"
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <limits.h>
#include <map>
#include <poll.h>
#include <semaphore.h>
#include <set>
#include <signal.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/stat.h> // add
#include <thread>
#define MAX_LINE 15000
#define MAXUSER 30
#define PROMPT "% "

const string WELCOME_MESSAGE = "****************************************\n"
                               "** Welcome to the information server. **\n"
                               "****************************************\n";

int null_fd;

UserInfo *userList;
sem_t *read_lock;
sem_t *write_lock;
array<int, 2> shared_pipe;
// Current process user FIFO <read, write> fd with [key] user.
// Each client process after fork would maintain its own user_pipe_fds
map<int, pair<int, int>> user_pipe_fds;

void initUserInfos(int idx) {
    userList[idx].isLogin = false;
    userList[idx].id = 0;
    userList[idx].ms_pipe = -1;
    strcpy(userList[idx].name, "(no name)");
    strcpy(userList[idx].ipPort, "");
}

void HandleInternalMsg(int fd_in, int user_id) {
    int sender_id;
    size_t msg_len;
    // shared_pipe[0] or ms_pipe[0]
    // received msg from pipe
    read(fd_in, &sender_id, sizeof(sender_id));
    read(fd_in, &msg_len, sizeof(msg_len));
    char msg_buf[msg_len + 1];
    msg_buf[msg_len] = '\0';
    read(fd_in, msg_buf, msg_len);
    string msg(msg_buf, msg_len);
    UserInfo *user = &userList[user_id];
    // parse message
    stringstream ss(msg);
    string arg;
    // first argument
    getline(ss, arg, ' ');

    if (arg == "login") {
        getline(ss, arg, ' '); // tcpinfo
        getline(ss, arg);      // msg
        cout << arg << endl;
    }

    if (arg == "exit") {
        // server clean up
        if (user_id == -1) {
            close(userList[sender_id].ms_pipe);
            initUserInfos(sender_id);
            getline(ss, arg); // msg
            cout << arg << endl;
        }
        if (user_id == sender_id) {
            // close exit user itself create pipe
            for (const auto &[key, val] : user_pipe_fds) {
                // use key, first, second here
                if (val.first != 0) {
                    close(val.first);
                }
                if (val.second != 0) {
                    close(val.second);
                }
            }
            exit(0);
        }
        // other user need to close the read, write FIFO corresponding to
        // leaving user as well
        if (user_pipe_fds[sender_id].first != 0) {
            close(user_pipe_fds[sender_id].first);
            string user_pipe_filename =
                "user_pipe/" + to_string(user_id) + "_" + to_string(sender_id);
            if (unlink(user_pipe_filename.c_str()) < 0) {
                cerr << "Error: failed to unlink user pipe" << endl;
            }
        }
        if (user_pipe_fds[sender_id].second != 0) {
            close(user_pipe_fds[sender_id].second);
            string user_pipe_filename =
                "user_pipe/" + to_string(sender_id) + "_" + to_string(user_id);
            if (unlink(user_pipe_filename.c_str()) < 0) {
                cerr << "Error: failed to unlink user pipe" << endl;
            }
        }

        user_pipe_fds.erase(sender_id);
        getline(ss, arg); // msg
        cout << arg << endl;
    }

    if (arg == "yell") {
        getline(ss, arg); // msg
        cout << arg << endl;
    }

    if (arg == "tell") {
        getline(ss, arg, ' ');      // receiver_id
        if (user_id == stoi(arg)) { // we are the receiver
            getline(ss, arg);       // msg
            cout << arg << endl;
        }
    }

    if (arg == "name") {
        getline(ss, arg); // msg
        cout << arg << endl;
    }

    if (arg == ">") {
        getline(ss, arg, ' '); // receiver_id
        int receiver_id = stoi(arg);
        if (user_id == receiver_id &&
            sender_id != user_id) { // if it's piped to us
            string user_pipe_filename = "./user_pipe/" +
                                        to_string(receiver_id) + '_' +
                                        to_string(sender_id);
            int read_fd =
                open(user_pipe_filename.c_str(), O_RDONLY | O_CLOEXEC);
            if (read_fd < 0) {
                string msg = "*** Error: Could not open pipe for reading by "
                             "receiver. ***\n";
                cout << msg << flush;
            }
            // sender: <read, write>
            user_pipe_fds[sender_id].first = read_fd;
        }
        // avoid printing kBash
        return;
    }

    if (arg == "<") {
        getline(ss, arg, ' '); // sender_id
        int pipe_out_id = stoi(arg);
        // sender_id is FIFO receiver, pipe_out_id is FIFO sender
        if (user_id == pipe_out_id &&
            sender_id != user_id) { // we are the pipe_out user
            // close it
            close(user_pipe_fds[sender_id].second);
            user_pipe_fds[sender_id].second = 0;
        }
        return;
    }

    if (arg == "user_pipe") {
        if (sender_id == user_id) {
            // avoid printing msg and kBash
            return;
        }
        while (getline(ss, arg)) { // msg (maybe more than one)
            cout << arg << endl << flush;
        }
    }
    // EX: ue1: yell abc, ue1 need print broadcast message before print prompt.
    if (sender_id == user_id) {
        cout << PROMPT << flush;
    }
}

int Shell(int user_id, PipeManager &pipe_manager,
          map<int, pair<int, int>> &user_pipe_fds) {
    string input;
    getline(cin, input);

    UserInfo *user = &userList[user_id];
    input.erase(input.find_last_not_of(" \n\r\t") +
                1); // remove trailing whitespace

    if (input.empty()) {
        cout << PROMPT << flush;
        return 0;
    }

    CommandParser parser(input, pipe_manager, user_id, read_lock, write_lock,
                         shared_pipe, userList, user_pipe_fds);
    bool need_bash = parser.processCommands();
    // Some command can't print prompt here, need receive broadcast message
    // first. EX: ue1: yell abc, ue1 need print broadcast message before print
    // prompt.
    if (need_bash)
        cout << PROMPT << flush;
    return 0;
}

class Server {
  private:
    int tcp_fd_;
    sockaddr_in tcp_addr_;

    bool listening_ = false;
    array<pollfd, 2> fds_;

    // void (*service_function_)(int user_id,
    //                           unordered_map<int, array<int, 2>> &pipeMap);

    string TcpIpPort(int tcp_fd) {
        sockaddr_in src_addr;
        socklen_t src_len = sizeof(src_addr);
        getpeername(tcp_fd, (sockaddr *)&src_addr, &src_len); // tcp

        stringstream ss;
        ss << inet_ntoa(src_addr.sin_addr) << ":" << ntohs(src_addr.sin_port);
        return ss.str();
    }

    int userLogin(int client_fd, sockaddr_in client_address, int pipe_fd) {
        char ipBuf[INET_ADDRSTRLEN];
        string ip = inet_ntoa(client_address.sin_addr);
        string clientIP = inet_ntop(AF_INET, &client_address.sin_addr, ipBuf,
                                    sizeof(clientIP));
        string clientPort = to_string(ntohs(client_address.sin_port));
        string ipPort = clientIP + ":" + clientPort;
        // Find the first available user slot
        for (int idx = 1; idx <= MAXUSER; idx++) {
            if (!userList[idx].isLogin) {
                PipeManager pipeManager;
                userList[idx].isLogin = true;
                userList[idx].id = idx;
                userList[idx].ms_pipe = pipe_fd;
                strcpy(userList[idx].ipPort, ipPort.c_str());
                return idx;
            }
        }
        return 0;
    }

    void handleConnection() {
        struct sockaddr_in client_address;
        socklen_t alen = sizeof(client_address);
        int client_fd =
            accept(tcp_fd_, (struct sockaddr *)&client_address, &alen);

        // Create pipe for server-client communication, [0]: client read, [1]: server write
        array<int, 2> ms_pipe;
        while (pipe2(ms_pipe.data(), O_CLOEXEC) == -1) {
            if (errno == EMFILE || errno == ENFILE) {
                wait(nullptr); // wait for any child process to release resource
            }
        }
        int user_id = userLogin(client_fd, client_address, ms_pipe[1]);
        UserInfo *user = &userList[user_id];

        pid_t child;
        while ((child = fork()) == -1) {
            if (errno == EAGAIN) {
                wait(nullptr); // wait for any child process to release resource
            }
        }

        if (child != 0) { // parent process
            close(client_fd);
            close(ms_pipe[0]);
            return;
        }

        /* child process ------------------------------- */
        close(tcp_fd_);
        close(shared_pipe[0]);
        // userList[i].ms_pipe is ms_pipe[1] close unused
        for (int i = 1; i <= MAXUSER; i++) {
            if (userList[i].isLogin) {
                close(userList[i].ms_pipe);
            }
        }
        setenv("PATH", "bin:.", 1);

        dup2(client_fd, STDIN_FILENO);
        dup2(client_fd, STDOUT_FILENO);
        dup2(client_fd, STDERR_FILENO);
        close(client_fd);

        cout << WELCOME_MESSAGE;

        sem_wait(write_lock);
        write(shared_pipe[1], &user_id, sizeof(user_id));
        string msg = "login " + string(userList[user_id].ipPort) +
                     " *** User \'" + string(userList[user_id].name) +
                     "\' entered from " + string(userList[user_id].ipPort) +
                     ". ***";
        size_t size = msg.size();
        write(shared_pipe[1], &size, sizeof(size));
        write(shared_pipe[1], msg.c_str(), size);
        sem_post(read_lock);

        fds_[0] = {.fd = ms_pipe[0], .events = POLL_IN, .revents = 0};
        fds_[1] = {.fd = STDIN_FILENO, .events = POLL_IN, .revents = 0};
        // Create each process used variable
        PipeManager pipe_manager;
        while (true) {
            poll(fds_.data(), fds_.size(), -1);

            for (pollfd pfd : fds_) {
                if (pfd.revents & POLL_IN) {
                    // handle internal message
                    if (pfd.fd == ms_pipe[0]) {
                        HandleInternalMsg(ms_pipe[0], user_id);
                    }

                    // handle client message
                    if (pfd.fd == STDIN_FILENO) {
                        Shell(user_id, pipe_manager, user_pipe_fds);
                    }
                }
            }
        }
    }

  public:
    Server(uint16_t port) {
        tcp_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        tcp_addr_.sin_family = AF_INET;
        tcp_addr_.sin_addr.s_addr = INADDR_ANY;
        tcp_addr_.sin_port = htons(port);

        int enable = 1;
        setsockopt(tcp_fd_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int));
        bind(tcp_fd_, (sockaddr *)&tcp_addr_, sizeof(tcp_addr_));
        cout << "[Server] Hosting on " << inet_ntoa(tcp_addr_.sin_addr)
             << ", port " << ntohs(tcp_addr_.sin_port) << '\n';

        fds_[0] = {.fd = shared_pipe[0], .events = POLL_IN, .revents = 0};
        fds_[1] = {.fd = tcp_fd_, .events = POLL_IN, .revents = 0};
    }

    ~Server() {
        shutdown(tcp_fd_, SHUT_RDWR);
        close(tcp_fd_);
        cout << "[Server] Shutdown\n";
    }

    void listen_for_message() {
        listening_ = true;
        listen(tcp_fd_, 1);

        while (listening_) {
            poll(fds_.data(), fds_.size(), -1);

            for (pollfd pfd : fds_) {
                if (pfd.revents & POLL_IN) {
                    if (pfd.fd == shared_pipe[0]) {
                        // broadcast internal message to every client
                        sem_wait(read_lock);
                        for (int i = 1; i <= MAXUSER; i++) {
                            if (userList[i].isLogin) {
                                tee(shared_pipe[0], userList[i].ms_pipe,
                                    INT_MAX, 0);
                            }
                        }
                        // handle internal message
                        HandleInternalMsg(shared_pipe[0], -1);
                        sem_post(write_lock);
                    }

                    // new tcp connection
                    if (pfd.fd == tcp_fd_) {
                        handleConnection();
                    }
                }
            }
        }
    }
};

void createSharedMemory() {
    // Create read, write lock shm
    int shm_fd = shm_open("/my_shm", O_CREAT | O_RDWR, 0666);
    ftruncate(shm_fd, sizeof(sem_t) * 2);
    read_lock = (sem_t *)mmap(nullptr, sizeof(sem_t) * 2,
                              PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    write_lock = read_lock + 1;
    sem_init(read_lock, 1, 0);
    sem_init(write_lock, 1, 1);
    // Create user_list shm
    int shm_fd_user_list =
        shm_open("/my_shm_user_list", O_CREAT | O_RDWR, 0666);
    ftruncate(shm_fd_user_list, sizeof(UserInfo) * (MAXUSER + 1));
    userList = (UserInfo *)mmap(nullptr, sizeof(UserInfo) * (MAXUSER + 1),
                                PROT_READ | PROT_WRITE, MAP_SHARED,
                                shm_fd_user_list, 0);
    close(shm_fd);
    close(shm_fd_user_list);
    return;
}

int main(int argc, char *argv[]) {
    // Prevent zombie processes by ignoring SIGCHLD
    signal(SIGCHLD, SIG_IGN);
    null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    // set up shared_pipe
    pipe2(shared_pipe.data(), O_CLOEXEC);
    createSharedMemory();
    shm_unlink("share-mem");
    mkdir("user_pipe", 0755);
    // Initialize shared data
    for (int i = 1; i <= MAXUSER; i++) {
        initUserInfos(i);
    }
    if (argc == 2) {
        Server server(atoi(argv[1]));
        server.listen_for_message();
    }
    // destroy semaphores
    sem_destroy(read_lock);
    sem_destroy(write_lock);
    return 0;
}