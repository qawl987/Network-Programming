#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>

using boost::asio::ip::tcp;
namespace beast = boost::beast;
namespace http = beast::http;

struct ServerInfo {
    std::string host;
    std::string port;
    std::string file_path;
};

void parse_query(std::string QUERY_STRING,
                 std::vector<ServerInfo> &servers_info) {
    std::stringstream ss(QUERY_STRING);
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
std::string output_shell(const std::string &session,
                         const std::string &content) {
    std::string safe_content = ReplaceNewlines(HtmlEscape(content));
    std::string shell_output = "<script>document.getElementById('" + session +
                               "').innerHTML += '" + safe_content +
                               "';</script>";
    return shell_output;
}

// Outputs bold command to browser
std::string output_command(const std::string &session,
                           const std::string &content) {
    std::string safe_content = ReplaceNewlines(HtmlEscape(content));
    std::string command_output = "<script>document.getElementById('" + session +
                                 "').innerHTML += '<b>" + safe_content +
                                 "</b>';</script>";
    return command_output;
}

void print_predefined_msg(std::ostream &response_stream,
                          std::vector<ServerInfo> servers_info) {
    response_stream << R"(<!DOCTYPE html>
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
        response_stream << "          <th scope=\"col\">" << s.host << ":"
                        << s.port << "</th>\n";
    }

    response_stream << R"(        </tr>
          </thead>
          <tbody>
            <tr>
    )";

    // Table cells with <pre> tags
    for (size_t i = 0; i < servers_info.size(); ++i) {
        response_stream << "          <td><pre id=\"s" << i
                        << "\" class=\"mb-0\"></pre></td>\n";
    }

    response_stream << R"(        </tr>
          </tbody>
        </table>
      </body>
    </html>
    )";
}

class session : public std::enable_shared_from_this<session> {
  public:
    session(tcp::socket socket, boost::asio::io_context &io_context)
        : socket_(std::move(socket)), io_context_(io_context) {}

    tcp::socket &socket() { return socket_; }

    void start() { do_read(); }

  private:
    boost::asio::io_context &io_context_;
    // connect to client socket
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

    void process_request();
};

class TcpClient : public std::enable_shared_from_this<TcpClient> {
  public:
    TcpClient(boost::asio::io_context &io_context, int column_number,
              std::shared_ptr<session> ptr)
        : resolver_(io_context), socket_(io_context),
          column_number(column_number), tcp_connection_(std::move(ptr)) {}

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
    std::shared_ptr<session> tcp_connection_;

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
        std::string shell_output = output_shell(session, ss.str());
        // output np_single response to terminal
        boost::asio::async_write(tcp_connection_->socket(),
                                 boost::asio::buffer(shell_output),
                                 [](beast::error_code, std::size_t) {});
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
        std::string command_output = output_command(session, cmd);
        // output cmd in txt
        boost::asio::async_write(tcp_connection_->socket(),
                                 boost::asio::buffer(command_output),
                                 [](beast::error_code, std::size_t) {});

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

void session::process_request() {
    std::string REQUEST_METHOD(request_.method_string());
    std::string target_str(request_.target());
    std::string REQUEST_URI = target_str;
    std::string QUERY_STRING;
    std::size_t pos = target_str.find('?');
    std::size_t query_position = target_str.find_first_of('?');
    std::string SERVER_PROTOCOL =
        request_.version() == 11 ? "HTTP/1.1" : "HTTP/1.0";
    if (query_position != std::string::npos) {
        QUERY_STRING = target_str.substr(query_position + 1);
        target_str = target_str.substr(1, query_position - 1);
    } else {
        target_str = target_str.substr(1);
    }

    if (target_str == "panel.cgi") {
        boost::asio::streambuf response;
        std::ostream response_stream(&response);

        response_stream << SERVER_PROTOCOL << " "
                        << static_cast<std::underlying_type_t<http::status>>(
                               http::status::ok)
                        << " " << http::status::ok << "\r\n";
        response_stream << "Content-type: text/html\r\n\r\n";

        response_stream
            << "<!DOCTYPE html>"
               "<html lang=\"en\">"
               "  <head>"
               "    <title>NP Project 4 Panel</title>"
               "    <link"
               "      rel=\"stylesheet\""
               "      "
               "href=\"https://cdn.jsdelivr.net/npm/bootstrap@4.5.3/dist/"
               "css/bootstrap.min.css\""
               "      "
               "integrity=\"sha384-TX8t27EcRE3e/"
               "ihU7zmQxVncDAy5uIKz4rEkgIXeMed4M0jlfIDPvg6uqKI2xXr2\""
               "      crossorigin=\"anonymous\""
               "    />"
               "    <link"
               "      "
               "href=\"https://fonts.googleapis.com/"
               "css?family=Source+Code+Pro\""
               "      rel=\"stylesheet\""
               "    />"
               "    <link"
               "      rel=\"icon\""
               "      type=\"image/png\""
               "      "
               "href=\"https://cdn4.iconfinder.com/data/icons/"
               "iconsimple-setting-time/512/dashboard-512.png\""
               "    />"
               "    <style>"
               "      * {"
               "        font-family: 'Source Code Pro', monospace;"
               "      }"
               "    </style>"
               "  </head>"
               "  <body class=\"bg-secondary pt-5\">"
               "    <form action=\"console.cgi\" method=\"GET\">"
               "      <table class=\"table mx-auto bg-light\" "
               "style=\"width: inherit\">"
               "        <thead class=\"thead-dark\">"
               "          <tr>"
               "            <th scope=\"col\">#</th>"
               "            <th scope=\"col\">Host</th>"
               "            <th scope=\"col\">Port</th>"
               "            <th scope=\"col\">Input File</th>"
               "          </tr>"
               "        </thead>"
               "        <tbody>"
               "          <tr>"
               "            <th scope=\"row\" "
               "class=\"align-middle\">Session 1</th>"
               "            <td>"
               "              <div class=\"input-group\">"
               "                <select name=\"h0\" "
               "class=\"custom-select\">"
               "                  <option></option><option "
               "value=\"nplinux1.cs.nycu.edu.tw\">nplinux1</option><option "
               "value=\"nplinux2.cs.nycu.edu.tw\">nplinux2</option><option "
               "value=\"nplinux3.cs.nycu.edu.tw\">nplinux3</option><option "
               "value=\"nplinux4.cs.nycu.edu.tw\">nplinux4</option><option "
               "value=\"nplinux5.cs.nycu.edu.tw\">nplinux5</option><option "
               "value=\"nplinux6.cs.nycu.edu.tw\">nplinux6</option><option "
               "value=\"nplinux7.cs.nycu.edu.tw\">nplinux7</option><option "
               "value=\"nplinux8.cs.nycu.edu.tw\">nplinux8</option><option "
               "value=\"nplinux9.cs.nycu.edu.tw\">nplinux9</option><option "
               "value=\"nplinux10.cs.nycu.edu.tw\">nplinux10</"
               "option><option "
               "value=\"nplinux11.cs.nycu.edu.tw\">nplinux11</"
               "option><option "
               "value=\"nplinux12.cs.nycu.edu.tw\">nplinux12</option>"
               "                </select>"
               "                <div class=\"input-group-append\">"
               "                  <span "
               "class=\"input-group-text\">.cs.nycu.edu.tw</span>"
               "                </div>"
               "              </div>"
               "            </td>"
               "            <td>"
               "              <input name=\"p0\" type=\"text\" "
               "class=\"form-control\" size=\"5\" />"
               "            </td>"
               "            <td>"
               "              <select name=\"f0\" class=\"custom-select\">"
               "                <option></option>"
               "                <option "
               "value=\"t1.txt\">t1.txt</option><option "
               "value=\"t2.txt\">t2.txt</option><option "
               "value=\"t3.txt\">t3.txt</option><option "
               "value=\"t4.txt\">t4.txt</option><option "
               "value=\"t5.txt\">t5.txt</option>"
               "              </select>"
               "            </td>"
               "          </tr>"
               "          <tr>"
               "            <th scope=\"row\" "
               "class=\"align-middle\">Session 2</th>"
               "            <td>"
               "              <div class=\"input-group\">"
               "                <select name=\"h1\" "
               "class=\"custom-select\">"
               "                  <option></option><option "
               "value=\"nplinux1.cs.nycu.edu.tw\">nplinux1</option><option "
               "value=\"nplinux2.cs.nycu.edu.tw\">nplinux2</option><option "
               "value=\"nplinux3.cs.nycu.edu.tw\">nplinux3</option><option "
               "value=\"nplinux4.cs.nycu.edu.tw\">nplinux4</option><option "
               "value=\"nplinux5.cs.nycu.edu.tw\">nplinux5</option><option "
               "value=\"nplinux6.cs.nycu.edu.tw\">nplinux6</option><option "
               "value=\"nplinux7.cs.nycu.edu.tw\">nplinux7</option><option "
               "value=\"nplinux8.cs.nycu.edu.tw\">nplinux8</option><option "
               "value=\"nplinux9.cs.nycu.edu.tw\">nplinux9</option><option "
               "value=\"nplinux10.cs.nycu.edu.tw\">nplinux10</"
               "option><option "
               "value=\"nplinux11.cs.nycu.edu.tw\">nplinux11</"
               "option><option "
               "value=\"nplinux12.cs.nycu.edu.tw\">nplinux12</option>"
               "                </select>"
               "                <div class=\"input-group-append\">"
               "                  <span "
               "class=\"input-group-text\">.cs.nycu.edu.tw</span>"
               "                </div>"
               "              </div>"
               "            </td>"
               "            <td>"
               "              <input name=\"p1\" type=\"text\" "
               "class=\"form-control\" size=\"5\" />"
               "            </td>"
               "            <td>"
               "              <select name=\"f1\" class=\"custom-select\">"
               "                <option></option>"
               "                <option "
               "value=\"t1.txt\">t1.txt</option><option "
               "value=\"t2.txt\">t2.txt</option><option "
               "value=\"t3.txt\">t3.txt</option><option "
               "value=\"t4.txt\">t4.txt</option><option "
               "value=\"t5.txt\">t5.txt</option>"
               "              </select>"
               "            </td>"
               "          </tr>"
               "          <tr>"
               "            <th scope=\"row\" "
               "class=\"align-middle\">Session 3</th>"
               "            <td>"
               "              <div class=\"input-group\">"
               "                <select name=\"h2\" "
               "class=\"custom-select\">"
               "                  <option></option><option "
               "value=\"nplinux1.cs.nycu.edu.tw\">nplinux1</option><option "
               "value=\"nplinux2.cs.nycu.edu.tw\">nplinux2</option><option "
               "value=\"nplinux3.cs.nycu.edu.tw\">nplinux3</option><option "
               "value=\"nplinux4.cs.nycu.edu.tw\">nplinux4</option><option "
               "value=\"nplinux5.cs.nycu.edu.tw\">nplinux5</option><option "
               "value=\"nplinux6.cs.nycu.edu.tw\">nplinux6</option><option "
               "value=\"nplinux7.cs.nycu.edu.tw\">nplinux7</option><option "
               "value=\"nplinux8.cs.nycu.edu.tw\">nplinux8</option><option "
               "value=\"nplinux9.cs.nycu.edu.tw\">nplinux9</option><option "
               "value=\"nplinux10.cs.nycu.edu.tw\">nplinux10</"
               "option><option "
               "value=\"nplinux11.cs.nycu.edu.tw\">nplinux11</"
               "option><option "
               "value=\"nplinux12.cs.nycu.edu.tw\">nplinux12</option>"
               "                </select>"
               "                <div class=\"input-group-append\">"
               "                  <span "
               "class=\"input-group-text\">.cs.nycu.edu.tw</span>"
               "                </div>"
               "              </div>"
               "            </td>"
               "            <td>"
               "              <input name=\"p2\" type=\"text\" "
               "class=\"form-control\" size=\"5\" />"
               "            </td>"
               "            <td>"
               "              <select name=\"f2\" class=\"custom-select\">"
               "                <option></option>"
               "                <option "
               "value=\"t1.txt\">t1.txt</option><option "
               "value=\"t2.txt\">t2.txt</option><option "
               "value=\"t3.txt\">t3.txt</option><option "
               "value=\"t4.txt\">t4.txt</option><option "
               "value=\"t5.txt\">t5.txt</option>"
               "              </select>"
               "            </td>"
               "          </tr>"
               "          <tr>"
               "            <th scope=\"row\" "
               "class=\"align-middle\">Session 4</th>"
               "            <td>"
               "              <div class=\"input-group\">"
               "                <select name=\"h3\" "
               "class=\"custom-select\">"
               "                  <option></option><option "
               "value=\"nplinux1.cs.nycu.edu.tw\">nplinux1</option><option "
               "value=\"nplinux2.cs.nycu.edu.tw\">nplinux2</option><option "
               "value=\"nplinux3.cs.nycu.edu.tw\">nplinux3</option><option "
               "value=\"nplinux4.cs.nycu.edu.tw\">nplinux4</option><option "
               "value=\"nplinux5.cs.nycu.edu.tw\">nplinux5</option><option "
               "value=\"nplinux6.cs.nycu.edu.tw\">nplinux6</option><option "
               "value=\"nplinux7.cs.nycu.edu.tw\">nplinux7</option><option "
               "value=\"nplinux8.cs.nycu.edu.tw\">nplinux8</option><option "
               "value=\"nplinux9.cs.nycu.edu.tw\">nplinux9</option><option "
               "value=\"nplinux10.cs.nycu.edu.tw\">nplinux10</"
               "option><option "
               "value=\"nplinux11.cs.nycu.edu.tw\">nplinux11</"
               "option><option "
               "value=\"nplinux12.cs.nycu.edu.tw\">nplinux12</option>"
               "                </select>"
               "                <div class=\"input-group-append\">"
               "                  <span "
               "class=\"input-group-text\">.cs.nycu.edu.tw</span>"
               "                </div>"
               "              </div>"
               "            </td>"
               "            <td>"
               "              <input name=\"p3\" type=\"text\" "
               "class=\"form-control\" size=\"5\" />"
               "            </td>"
               "            <td>"
               "              <select name=\"f3\" class=\"custom-select\">"
               "                <option></option>"
               "                <option "
               "value=\"t1.txt\">t1.txt</option><option "
               "value=\"t2.txt\">t2.txt</option><option "
               "value=\"t3.txt\">t3.txt</option><option "
               "value=\"t4.txt\">t4.txt</option><option "
               "value=\"t5.txt\">t5.txt</option>"
               "              </select>"
               "            </td>"
               "          </tr>"
               "          <tr>"
               "            <th scope=\"row\" "
               "class=\"align-middle\">Session 5</th>"
               "            <td>"
               "              <div class=\"input-group\">"
               "                <select name=\"h4\" "
               "class=\"custom-select\">"
               "                  <option></option><option "
               "value=\"nplinux1.cs.nycu.edu.tw\">nplinux1</option><option "
               "value=\"nplinux2.cs.nycu.edu.tw\">nplinux2</option><option "
               "value=\"nplinux3.cs.nycu.edu.tw\">nplinux3</option><option "
               "value=\"nplinux4.cs.nycu.edu.tw\">nplinux4</option><option "
               "value=\"nplinux5.cs.nycu.edu.tw\">nplinux5</option><option "
               "value=\"nplinux6.cs.nycu.edu.tw\">nplinux6</option><option "
               "value=\"nplinux7.cs.nycu.edu.tw\">nplinux7</option><option "
               "value=\"nplinux8.cs.nycu.edu.tw\">nplinux8</option><option "
               "value=\"nplinux9.cs.nycu.edu.tw\">nplinux9</option><option "
               "value=\"nplinux10.cs.nycu.edu.tw\">nplinux10</"
               "option><option "
               "value=\"nplinux11.cs.nycu.edu.tw\">nplinux11</"
               "option><option "
               "value=\"nplinux12.cs.nycu.edu.tw\">nplinux12</option>"
               "                </select>"
               "                <div class=\"input-group-append\">"
               "                  <span "
               "class=\"input-group-text\">.cs.nycu.edu.tw</span>"
               "                </div>"
               "              </div>"
               "            </td>"
               "            <td>"
               "              <input name=\"p4\" type=\"text\" "
               "class=\"form-control\" size=\"5\" />"
               "            </td>"
               "            <td>"
               "              <select name=\"f4\" class=\"custom-select\">"
               "                <option></option>"
               "                <option "
               "value=\"t1.txt\">t1.txt</option><option "
               "value=\"t2.txt\">t2.txt</option><option "
               "value=\"t3.txt\">t3.txt</option><option "
               "value=\"t4.txt\">t4.txt</option><option "
               "value=\"t5.txt\">t5.txt</option>"
               "              </select>"
               "            </td>"
               "          </tr>"
               "          <tr>"
               "            <td colspan=\"3\"></td>"
               "            <td>"
               "              <button type=\"submit\" class=\"btn btn-info "
               "btn-block\">Run</button>"
               "            </td>"
               "          </tr>"
               "        </tbody>"
               "      </table>"
               "    </form>"
               "  </body>"
               "</html>";
        auto self = shared_from_this();
        boost::asio::async_write(socket_, response,
                                 [](beast::error_code, std::size_t) {});
        return;
    }

    if (target_str == "console.cgi") {
        boost::asio::streambuf response;
        std::ostream response_stream(&response);
        std::vector<ServerInfo> servers_info;
        parse_query(QUERY_STRING, servers_info);
        response_stream << SERVER_PROTOCOL << " "
                        << static_cast<std::underlying_type_t<http::status>>(
                               http::status::ok)
                        << " " << http::status::ok << "\r\n";
        response_stream << "Content-type: text/html\r\n\r\n";
        print_predefined_msg(response_stream, servers_info);
        auto self = shared_from_this();
        boost::asio::async_write(
            socket_, response,
            [self, servers_info](beast::error_code, std::size_t) {
                for (size_t i = 0; i < servers_info.size(); i++) {
                    auto client =
                        std::make_shared<TcpClient>(self->io_context_, i, self);
                    client->async_connect(servers_info[i]);
                }
            });
        return;
    }
}

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
                    std::make_shared<session>(std::move(socket), io_context_)
                        ->start();
                }
                do_accept();
            });
    }
};

int main(int argc, char *argv[]) {
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