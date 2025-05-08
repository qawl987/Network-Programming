#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <utility>
#include <vector>

using boost::asio::ip::tcp;
namespace beast = boost::beast;
namespace http = beast::http;

struct ServerInfo {
    std::string host;
    std::string port;
    std::string file_path;
};

std::vector<ServerInfo> servers_info;

void parse_query() {
    std::stringstream ss(getenv("QUERY_STRING"));
    std::map<std::string, std::string> kv_pairs;
    std::string token;
    while (std::getline(ss, token, '&')) {
        auto pos = token.find('=');
        if (pos != std::string::npos) {
            std::string key = token.substr(0, pos);
            std::string value = token.substr(pos + 1);
            kv_pairs[key] = value;
        }
    }
    for (int i = 0; i < 5; ++i) {
        std::string h_key = "h" + std::to_string(i);
        std::string p_key = "p" + std::to_string(i);
        std::string f_key = "f" + std::to_string(i);

        std::string host = kv_pairs[h_key];
        std::string port = kv_pairs[p_key];
        std::string file = kv_pairs[f_key];

        if (!host.empty() && !port.empty() && !file.empty()) {
            servers_info.push_back(ServerInfo{host, port, file});
        }
    }
}

// Escapes HTML special characters
std::string HtmlEscape(const std::string &input) {
    std::ostringstream escaped;
    for (char c : input) {
        switch (c) {
        case '&':
            escaped << "&amp;";
            break;
        case '<':
            escaped << "&lt;";
            break;
        case '>':
            escaped << "&gt;";
            break;
        case '\"':
            escaped << "&quot;";
            break;
        case '\'':
            escaped << "&#39;";
            break;
        default:
            escaped << c;
        }
    }
    return escaped.str();
}

// Replaces '\n' with "&NewLine;"
std::string ReplaceNewlines(const std::string &input) {
    std::string result;
    for (char c : input) {
        if (c == '\n')
            result += "&NewLine;";
        else
            result += c;
    }
    return result;
}

// Outputs shell output to browser via JavaScript
void output_shell(const std::string &session, const std::string &content) {
    std::string safe_content = ReplaceNewlines(HtmlEscape(content));
    std::cout << "<script>document.getElementById('" << session
              << "').innerHTML += '" << safe_content << "';</script>"
              << std::flush;
}

// Outputs bold command to browser
void output_command(const std::string &session, const std::string &content) {
    std::string safe_content = ReplaceNewlines(HtmlEscape(content));
    std::cout << "<script>document.getElementById('" << session
              << "').innerHTML += '<b>" << safe_content << "</b>';</script>"
              << std::flush;
}

void print_predefined_msg() {
    std::cout << "Content-type: text/html\r\n\r\n";
    std::cout << R"(<!DOCTYPE html>
    <html lang="en">
      <head>
        <meta charset="UTF-8" />
        <title>NP Project 4 Sample Console</title>
        <link
          rel="stylesheet"
          href="https://cdn.jsdelivr.net/npm/bootstrap@4.5.3/dist/css/bootstrap.min.css"
          integrity="sha384-TX8t27EcRE3e/ihU7zmQxVncDAy5uIKz4rEkgIXeMed4M0jlfIDPvg6uqKI2xXr2"
          crossorigin="anonymous"
        />
        <link
          href="https://fonts.googleapis.com/css?family=Source+Code+Pro"
          rel="stylesheet"
        />
        <link
          rel="icon"
          type="image/png"
          href="https://cdn0.iconfinder.com/data/icons/small-n-flat/24/678068-terminal-512.png"
        />
        <style>
          * {
            font-family: 'Source Code Pro', monospace;
            font-size: 1rem !important;
          }
          body {
            background-color: #FFFFFF;
          }
          pre {
            color: #cccccc;
          }
          b {
            color: #01b468;
          }
        </style>
      </head>
      <body>
        <table class="table table-dark table-bordered">
          <thead>
            <tr>
    )";

    // Table headers
    for (const auto &s : servers_info) {
        std::cout << "          <th scope=\"col\">" << s.host << ":" << s.port
                  << "</th>\n";
    }

    std::cout << R"(        </tr>
          </thead>
          <tbody>
            <tr>
    )";

    // Table cells with <pre> tags
    for (size_t i = 0; i < servers_info.size(); ++i) {
        std::cout << "          <td><pre id=\"s" << i
                  << "\" class=\"mb-0\"></pre></td>\n";
    }

    std::cout << R"(        </tr>
          </tbody>
        </table>
      </body>
    </html>
    )";
}

class TcpClient : public std::enable_shared_from_this<TcpClient> {
  public:
    TcpClient(boost::asio::io_context &io_context, int column_number)
        : resolver_(io_context), socket_(io_context),
          column_number(column_number) {}

    void async_connect(ServerInfo server_info) {
        auto self = shared_from_this();
        resolver_.async_resolve(
            server_info.host, server_info.port,
            [self, server_info](beast::error_code,
                                const tcp::resolver::iterator &it) {
                self->socket_.async_connect(
                    it->endpoint(), [self, server_info](beast::error_code) {
                        self->ip_port_ =
                            server_info.host + ':' + server_info.port;
                        self->file_.open("test_case/" + server_info.file_path,
                                         std::fstream::in);
                        self->async_read();
                    });
            });
    }

  private:
    int column_number;
    tcp::resolver resolver_;
    tcp::socket socket_;
    std::string ip_port_;
    std::fstream file_;
    http::request<http::dynamic_body> request_;
    boost::asio::streambuf streambuf_;

    void async_read() {
        auto self(shared_from_this());
        boost::asio::async_read(socket_, streambuf_,
                                boost::asio::transfer_at_least(1),
                                [self](beast::error_code, std::size_t) {
                                    self->process_response();
                                });
    }

    void process_response() {
        std::istream input(&streambuf_);
        std::stringstream ss;
        ss << input.rdbuf() << std::flush;

        std::string session = "s" + std::to_string(column_number);
        output_shell(session, ss.str());
        if (ss.str().find('%') == std::string::npos) {
            async_read();
            return;
        }

        std::string cmd;
        std::getline(file_, cmd);
        if (cmd.back() == '\r') {
            cmd.pop_back();
        }
        cmd.append(1, '\n');
        output_command(session, cmd);

        auto self = shared_from_this();
        boost::asio::async_write(socket_, boost::asio::buffer(cmd),
                                 [self, cmd](beast::error_code, std::size_t) {
                                     if (cmd == "exit\n") {
                                         self->socket_.close();
                                     } else {
                                         self->async_read();
                                     }
                                 });
    }
};

int main(int argc, char *argv[]) {
    signal(SIGCHLD, SIG_IGN);
    parse_query();
    print_predefined_msg();

    try {
        boost::asio::io_context io_context;
        // Shared_ptr create here to prevent the client object lifetime end
        // before the register async event started.
        // Shared_ptr create object on heap.
        for (size_t i = 0; i < servers_info.size(); i++) {
            auto client = std::make_shared<TcpClient>(io_context, i);
            client->async_connect(servers_info[i]);
        }
        io_context.run();
    } catch (std::exception &e) {
        std::cerr << e.what() << std::endl;
    }

    return 0;
}
