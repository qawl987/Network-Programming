#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <utility>

using boost::asio::ip::tcp;
namespace beast = boost::beast;
namespace http = beast::http;

class session : public std::enable_shared_from_this<session> {
  public:
    // socket pass as rvalue below, so here socket is again a lvalue, socket_
    // need to steal it again
    session(tcp::socket socket, boost::asio::io_context &io_context)
        : socket_(std::move(socket)), io_context_(io_context) {}

    void start() { do_read(); }

  private:
    boost::asio::io_context &io_context_;
    tcp::socket socket_;
    http::request<http::dynamic_body> request_;
    enum { max_length = 1024 };
    beast::flat_buffer buffer_{8192};

    void do_read() {
        auto self(shared_from_this());
        http::async_read(
            socket_, buffer_, request_,
            [self](beast::error_code, std::size_t bytes_transferred) {
                boost::ignore_unused(bytes_transferred);
                self->process_request();
            });
    }

    void process_request() {
        io_context_.notify_fork(boost::asio::io_context::fork_prepare);

        pid_t child;
        while ((child = fork()) == -1) {
            if (errno == EAGAIN) {
                wait(nullptr); // wait for any child process to release resource
            }
        }
        if (child != 0) { // parent process
            io_context_.notify_fork(boost::asio::io_context::fork_parent);
            return;
        }
        io_context_.notify_fork(boost::asio::io_context::fork_child);

        auto args = std::make_unique<char *[]>(2);
        args[1] = nullptr;

        std::string REQUEST_METHOD(request_.method_string());
        std::string target_str(request_.target());

        std::string REQUEST_URI = target_str;
        std::string QUERY_STRING;
        std::size_t pos = target_str.find('?');
        std::string target_path;
        // Some periodically connection would trigger Exception:
        // basic_string_view::substr.typically caused by calling substr(pos,
        // len) with pos > size() on a boost::string_view (or similarly,
        // std::string_view). That is undefined behavior and will throw an
        // exception or crash, depending on the implementation.
        if (!target_str.empty() && target_str[0] == '/') {
            if (pos != std::string::npos) {
                QUERY_STRING = target_str.substr(pos + 1);
                if (pos > 1) {
                    target_path = "./" + target_str.substr(1, pos - 1);
                } else {
                    target_path = "./";
                }
            } else {
                target_path = "./" + target_str.substr(1);
            }
        } else {
            target_path = "./";
        }

        args[0] = strdup(target_path.c_str());

        unsigned int major = request_.version() / 10;
        unsigned int minor = request_.version() % 10;
        std::string SERVER_PROTOCOL =
            "HTTP/" + std::to_string(major) + '.' + std::to_string(minor);
        std::string HTTP_HOST;
        auto host_it = request_.find(boost::beast::http::field::host);
        if (host_it != request_.end()) {
            HTTP_HOST = std::string(host_it->value());
        }
        setenv("REQUEST_METHOD", REQUEST_METHOD.c_str(), 1);
        setenv("REQUEST_URI", REQUEST_URI.c_str(), 1);
        setenv("QUERY_STRING", QUERY_STRING.c_str(), 1);
        setenv("SERVER_PROTOCOL", SERVER_PROTOCOL.c_str(), 1);
        setenv("HTTP_HOST", HTTP_HOST.c_str(), 1);
        setenv("SERVER_ADDR",
               socket_.local_endpoint().address().to_string().c_str(), 1);
        setenv("SERVER_PORT",
               std::to_string(socket_.local_endpoint().port()).c_str(), 1);
        setenv("REMOTE_ADDR",
               socket_.remote_endpoint().address().to_string().c_str(), 1);
        setenv("REMOTE_PORT",
               std::to_string(socket_.remote_endpoint().port()).c_str(), 1);

        dup2(socket_.native_handle(), STDIN_FILENO);
        dup2(socket_.native_handle(), STDOUT_FILENO);
        close(socket_.native_handle());

        // Http Response Status-Line
        std::cout << SERVER_PROTOCOL << " "
                  << static_cast<std::underlying_type_t<http::status>>(
                         http::status::ok)
                  << " " << http::status::ok << "\r\n"
                  << std::flush;

        if (execv(args[0], args.get()) == -1) {
            perror(args[0]);
            exit(0);
        }
    }
};

class server {
  public:
    server(boost::asio::io_context &io_context, short port)
        : io_context_(io_context),
          acceptor_(io_context, tcp::endpoint(tcp::v4(), port)) {
        do_accept();
    }

  private:
    tcp::acceptor acceptor_;
    boost::asio::io_context &io_context_;

    void do_accept() {
        acceptor_.async_accept(
            [this](boost::system::error_code ec, tcp::socket socket) {
                if (!ec) {
                    // Move lvalue socket to rvalue, so session object can steal
                    // this socket anything like fd, constructor
                    std::make_shared<session>(std::move(socket), io_context_)
                        ->start();
                }
                do_accept();
            });
    }
};

int main(int argc, char *argv[]) {
    signal(SIGCHLD, SIG_IGN);
    try {
        if (argc != 2) {
            std::cerr << "Usage: http_server <port>\n";
            return 1;
        }

        boost::asio::io_context io_context;
        server s(io_context, std::atoi(argv[1]));
        io_context.run();
    } catch (std::exception &e) {
        std::cerr << "Exception: " << e.what() << "\n";
    }

    return 0;
}
