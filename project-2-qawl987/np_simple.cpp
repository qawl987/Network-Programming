#include "npshell_simple.cpp"
#include <arpa/inet.h>
#define MAX_LINE 15000
#define MAXUSER 30

int main(int argc, char *argv[]) {
    int SERV_TCP_PORT = std::atoi(argv[1]);
    int listenfd, connfd;
    pid_t childpid;
    socklen_t clilen;
    struct sockaddr_in cli_addr, serv_addr;

    // Create listening socket
    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("server: can't open stream socket");
        exit(1);
    }

    // Allow address reuse - helpful for restarting server quickly
    int optval = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &optval,
                   sizeof(optval)) < 0) {
        perror("setsockopt(SO_REUSEADDR) failed");
    }

    // Prepare the sockaddr_in structure
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(SERV_TCP_PORT);

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

    cerr << "Server listening on port " << SERV_TCP_PORT << "..." << endl;

    // Prevent zombie processes by ignoring SIGCHLD
    signal(SIGCHLD, SIG_IGN);

    while (1) { // Main accept loop
        clilen = sizeof(cli_addr);
        connfd = accept(listenfd, (struct sockaddr *)&cli_addr, &clilen);

        if (connfd < 0) {
            if (errno == EINTR)
                continue;
            else {
                perror("server: accept error");
                // Consider whether to continue or exit depending on the error
                continue; // Continue for now
            }
        }

        cerr << "Connection accepted from " << inet_ntoa(cli_addr.sin_addr)
             << ":" << ntohs(cli_addr.sin_port) << endl;

        if ((childpid = fork()) < 0) {
            perror("server: fork error");
            close(connfd);          // Close connection if fork fails
        } else if (childpid == 0) { /* --- Child process --- */
            close(listenfd); /* Child doesn't need the listening socket */

            // ****** KEY CHANGE: Redirect STDOUT and STDERR to the client
            if (dup2(connfd, STDOUT_FILENO) < 0) {
                perror("dup2 stdout error");
                exit(1);
            }
            if (dup2(connfd, STDERR_FILENO) < 0) {
                perror("dup2 stderr error");
                exit(1);
            }
            // After duplication, we don't need the original connfd in the child
            // for standard I/O anymore. `cout`, `cerr`, `printf`, etc. will now
            // use the file descriptors 1 and 2, which point to the socket. We
            // WILL still need connfd for recv(). close(connfd); // DO NOT CLOSE
            // connfd HERE - needed for recv!

            setenv("PATH", "bin:.", 1);

            PipeManager pipe_manager;
            char inputBuffer[MAX_LINE + 1]; // +1 for null terminator
            ssize_t n;

            cout << "% " << flush;

            // --- Client command processing loop ---
            while (true) {
                memset(inputBuffer, 0, sizeof(inputBuffer));
                n = recv(connfd, inputBuffer, MAX_LINE, 0);

                if (n < 0) {
                    perror("recv error");
                    break;
                } else if (n == 0) {
                    cerr << "Client disconnected."
                         << endl; // Log on server console
                    break;        // Exit loop on client disconnection
                }

                string input(inputBuffer);
                input.erase(input.find_last_not_of("\r\n") +
                            1); // Trim trailing whitespace/newlines

                if (input.empty()) {
                    cout << "% " << flush; // Show prompt again
                    continue;
                }

                // Allow server-side exit command for the child process
                if (input == "exit") {
                    break; // Exit the loop cleanly
                }

                // --- Process the command using npshell ---
                try {
                    CommandParser parser(input, pipe_manager);
                    parser.processCommands();
                } catch (const std::exception &e) {
                    // Basic error handling for exceptions during
                    // parsing/execution
                    cerr << "Error processing command: " << e.what() << endl;
                } catch (...) {
                    cerr << "Unknown error processing command." << endl;
                }

                // --- Send next prompt ---
                cout << "% " << flush; // Use cout, it goes to the socket
            }

            cerr << "Child process terminating for client."
                 << endl;  // Log on server console
            close(connfd); // Close the connection socket before exiting child
            exit(0);       // Terminate child process
            /* --- End Child process --- */

        } else { /* --- Parent process --- */
            // Parent doesn't need the connection socket after fork
            close(connfd);
            /* Parent loops to accept next connection */
        }
    } // End while(1) accept loop

    close(listenfd); // Close listening socket when server loop exits (though it
                     // won't in this structure)
    return 0;
}