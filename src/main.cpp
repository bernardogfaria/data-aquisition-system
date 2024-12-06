#include <boost/asio.hpp>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <ctime>
#include <utility>
#include <filesystem>

using boost::asio::ip::tcp;

// ** Definições Globais e Constantes **
std::vector<std::string> registered_ids;
const std::string INVALID_SENSOR_MESSAGE = "ERROR|INVALID_SENSOR_ID\r\n";

// Prototipação de Funções
void store_log_data(const std::string& log);
std::string generate_response(const std::string& request);
bool is_id_registered(const std::vector<std::string>& ids, const std::string& id);
void log_error(const std::string& error_msg);

// ** Estruturas de Dados **
#pragma pack(push, 1)
struct LogEntry {
    char sensor_id[32];
    std::time_t timestamp;
    double reading;
};
#pragma pack(pop)

// ** Funções de Conversão de Tempo **
std::time_t parse_time(const std::string& time_str) {
    std::tm tm = {};
    std::istringstream stream(time_str);
    stream >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    return std::mktime(&tm);
}

std::string format_time(std::time_t time) {
    std::tm* tm = std::localtime(&time);
    std::ostringstream stream;
    stream << std::put_time(tm, "%Y-%m-%dT%H:%M:%S");
    return stream.str();
}

// ** Funções de Parsing **
void parse_log_entry(const std::string& log_msg, LogEntry& entry) {
    std::istringstream stream(log_msg);
    std::string field;

    std::getline(stream, field, '|'); // Ignora "LOG"
    std::getline(stream, field, '|'); // ID do sensor
    std::strncpy(entry.sensor_id, field.c_str(), sizeof(entry.sensor_id));

    std::getline(stream, field, '|'); // Timestamp
    entry.timestamp = parse_time(field);

    std::getline(stream, field, '|'); // Valor da leitura
    entry.reading = std::stod(field);
}

void parse_request(const std::string& request, char (&sensor_id)[32], int& record_count) {
    std::istringstream stream(request);
    std::string field;

    std::getline(stream, field, '|'); // Ignora "GET"
    std::getline(stream, field, '|'); // ID do sensor
    std::strncpy(sensor_id, field.c_str(), sizeof(sensor_id));

    std::getline(stream, field, '|'); // Número de registros
    record_count = std::stoi(field);
}

// ** Sessão do Cliente **
class Session : public std::enable_shared_from_this<Session> {
public:
    explicit Session(tcp::socket socket) : client_socket(std::move(socket)) {}

    void start() { listen_for_messages(); }

private:
    void listen_for_messages() {
        auto self = shared_from_this();
        boost::asio::async_read_until(client_socket, input_buffer, "\r\n",
            [this, self](boost::system::error_code ec, std::size_t bytes_transferred) {
                if (!ec) {
                    std::istream input_stream(&input_buffer);
                    std::string received_msg((std::istreambuf_iterator<char>(input_stream)), {});

                    std::cout << "Mensagem recebida: " << received_msg << std::endl;
                    handle_message(received_msg);
                }
            });
    }

    void handle_message(const std::string& msg) {
        auto command = msg.substr(0, 3);

        if (command == "LOG") {
            LogEntry entry;
            parse_log_entry(msg, entry);
            if (!is_id_registered(registered_ids, entry.sensor_id)) {
                registered_ids.push_back(entry.sensor_id);
            }
            store_log_data(msg);
        } else if (command == "GET") {
            char sensor_id[32];
            int num_records;
            parse_request(msg, sensor_id, num_records);
            if (!is_id_registered(registered_ids, sensor_id)) {
                send_message(INVALID_SENSOR_MESSAGE);
            } else {
                send_message(generate_response(msg));
            }
        }

        listen_for_messages();
    }

    void send_message(const std::string& msg) {
        auto self = shared_from_this();
        boost::asio::async_write(client_socket, boost::asio::buffer(msg),
            [this, self](boost::system::error_code ec, std::size_t) {
                if (!ec) {
                    listen_for_messages();
                }
            });
    }

    tcp::socket client_socket;
    boost::asio::streambuf input_buffer;
};

// ** Servidor TCP **
class Server {
public:
    Server(boost::asio::io_context& io_ctx, unsigned short port)
        : acceptor(io_ctx, tcp::endpoint(tcp::v4(), port)) {
        initiate_accept();
    }

private:
    void initiate_accept() {
        acceptor.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
            if (!ec) {
                std::make_shared<Session>(std::move(socket))->start();
            }
            initiate_accept();
        });
    }

    tcp::acceptor acceptor;
};

// ** Funções Auxiliares **
void store_log_data(const std::string& log) {
    LogEntry entry;
    parse_log_entry(log, entry);
    std::string filename = entry.sensor_id + std::string(".dat");
    std::ofstream file(filename, std::ios::binary | std::ios::app);

    if (file) {
        file.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
        file.close();
    } else {
        log_error("Erro ao salvar dados do log.");
    }
}

std::string generate_response(const std::string& request) {
    char sensor_id[32];
    int num_records;
    parse_request(request, sensor_id, num_records);

    std::string response;
    std::ifstream file(std::string(sensor_id) + ".dat", std::ios::binary);

    if (file) {
        LogEntry entry;
        while (file.read(reinterpret_cast<char*>(&entry), sizeof(entry)) && num_records-- > 0) {
            response += "Sensor: " + std::string(entry.sensor_id) + 
                        ", Tempo: " + format_time(entry.timestamp) + 
                        ", Valor: " + std::to_string(entry.reading) + "\n";
        }
    } else {
        log_error("Erro ao abrir arquivo para leitura.");
    }

    return response;
}

bool is_id_registered(const std::vector<std::string>& ids, const std::string& id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

void log_error(const std::string& error_msg) {
    std::cerr << error_msg << std::endl;
}

// ** Função Principal **
int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Uso: server <porta>\n";
        return EXIT_FAILURE;
    }

    boost::asio::io_context io_ctx;
    Server server(io_ctx, std::atoi(argv[1]));
    io_ctx.run();

    return EXIT_SUCCESS;
}
