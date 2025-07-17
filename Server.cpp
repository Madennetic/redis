#include <iostream>
#include <cstdlib>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <thread>
#include <vector>
#include <regex>
#include <chrono>
#include <algorithm>

//https://redis.io/docs/latest/operate/oss_and_stack/install/archive/install-redis/install-redis-on-linux/
// Redis documentation for starting and stopping

// This is a debugging function for printing literal strings without \n etc...
void print_literal(const std::string& str) {
  for (char c : str) {
    switch (c) {
      case '\n': std::cout << "\\n"; break;
      case '\r': std::cout << "\\r"; break;
      case '\t': std::cout << "\\t"; break;
      case '\\': std::cout << "\\\\"; break;
      default: std::cout << c; break;
    }
  }
  std::cout << std::endl;
}

// This function finds the first number in a string and returns it
int primary_int_finder(const std::string& str) {
  std::string num_str;

  for (const char& c : str) {
    if (std::isdigit(c)) {
      num_str += c;
    }
    else if (!num_str.empty()) {
      break;
    }
  }

  return stoi(num_str);
}

// This function is used for taking in RESP protocol commands and parsing them
// It returns a vector of the commands
std::vector<std::string> parse(std::string str) {

  std::vector<std::string> parsed_commands;
  int jump = 3 + str.find("\r\n");              // The jump is rough, but since the RESP protocol is just a pattern
                                                  // you can just 'jump' from one part to the next since you know how long each part is
                                                  // The jump is used to navigate the RESP string properly, and tracks the index
  if (str.substr(0, 1) == "*") {
    int words = primary_int_finder(str);          // This find how many elements need to be parsed
    for (int i = 0; i < words; i++) {             //should be a while true in case data is longer than 1024
                                                  // this is assuming that these are all bulk strings with $

      int current_str_length = primary_int_finder(str.substr(jump));        // This finds the length of the current word being parsed
      jump = (str.substr(jump)).find("\r\n") + jump + 2;                     // This sets the proper jump and start index for the word to be parsed
      parsed_commands.push_back(str.substr(jump, current_str_length));     // This adds the parsed command using the previous two lines
      jump += current_str_length + 2;                                             // This properly sets the jump for the next word to be parsed
    }

    return parsed_commands;

  }
  else {                                          // This is for future non-bulk string implementation
    std::cerr << "We don't know you yet" << std::endl;
    return {};
  }
  return {};
}

// checks that the final characters of the data are the correct ones
// This can still break which isn't good, if \r\n just happen to line up
int resp_check(const std::string& str) {

  if (str.size() >= 2 && str.substr(str.size() - 2, 2) == "\r\n") {
    return 1;
  }
  return -1;
}

// This function makes strings uppercase
std::string to_upper(std::string str) {
  std::transform(std::begin(str), std::end(str),std::begin(str), ::toupper);
  return str;
}

// This function is for handling multiple clients at once
// This function will be a detached thread that runs
// It receives the client, and loops a response until close/error
// Then it closes the client and ends the function then and there
void thread_socket(int client_fd) {

  char buffer[1024];                                            // Sets how much data can be taken at once
  std::string data;

  struct set_info {                                             // Struct for holding the set information (key value, expiry)
    std::string value;
    std::chrono::steady_clock::time_point expiry;
  };
  std::map<std::string, set_info> sets;                         // This tracks the sets that are set using SET

  while (true) {                                                // Outer loop for handling whole interaction

                                                                // recv returns 0 at disconnect, -1 at error, and a number of bytes when proper
                                                                // If the number is higher than 1024 it loops
                                                                // It stores the received info in buffer and adds it to data

    while (true) {                                              // Inner loop for receiving data
      int bytes = recv(client_fd, buffer, sizeof(buffer), 0);
      if (bytes == 0) {
        std::cerr << "Client disconnected" << std::endl;
        close(client_fd);
        return;
      }
      else if (bytes < 0) {
        std::cerr << "Client error" << std::endl;
        close(client_fd);
        return;
      }
      data.append(buffer, bytes);
      if (resp_check(data) == 1) break;
    }

    std::vector<std::string> parsed_commands = parse(data);

    for (int i = 0; i < parsed_commands.size(); i++) {            // This loops through the commands (This loop is actually pretty useless, because it will only ever go through the first loop)
                                                                  // It compares the parsed commands to known commands and properly responds
      // print_literal(parsed_commands[i]); // This is for debugging

      // This is for proper case-insensitive implementation
      std::string current_command = to_upper(parsed_commands[i]);

      if (current_command == "PING") {
        std::string response = "+PONG\r\n";
        send(client_fd, response.c_str(), response.size(), 0);
        break;
      }

      else if (current_command == "ECHO") { // Echos next parsed command back into the console
        if (i == parsed_commands.size() - 1) {
          std::string response = "+ERR wrong number of arguments for 'ECHO' command\r\n";
          send(client_fd, response.c_str(), response.size(), 0);
          break;
        }

        std::string response = "+" + parsed_commands[i + 1] + "\r\n";
        send(client_fd, response.c_str(), response.size(), 0);
        break;
      }

      else if (current_command == "SET") { // Sets a map key using the next command then the one after that as a value
        if (i == parsed_commands.size() - 1 || i == parsed_commands.size() - 2 || i == parsed_commands.size() - 4) { // 3rd last & 5th last are valid
          std::string response = "+ERR wrong number of arguments for 'SET' command\r\n";
          send(client_fd, response.c_str(), response.size(), 0);
          break;
        }

        if (i <= parsed_commands.size() - 5) {       // For two or more command sets
          if (to_upper(parsed_commands[i + 3]) == "PX") {      // For expiry
            sets[parsed_commands[i + 1]] = set_info{
              parsed_commands[i + 2],
              std::chrono::steady_clock::now() + std::chrono::milliseconds(stoi(parsed_commands[i + 4]))};
            std::string response = "+OK\r\n";
            send(client_fd, response.c_str(), response.size(), 0);
            break;
          }
        }

        sets[parsed_commands[i + 1]] = set_info{    // For basic sets with no expiry
          parsed_commands[i + 2],
          std::chrono::steady_clock::time_point::max()};
        std::string response = "+OK\r\n";
        send(client_fd, response.c_str(), response.size(), 0);
        break;
      }

      else if (current_command == "GET"){ // Prints the value for the next command given, which is a key
        if (i == parsed_commands.size() - 1) {
          std::string response = "+ERR wrong number of arguments for 'GET'\r\n";
          send(client_fd, response.c_str(), response.size(), 0);
          break;
        }
        
        if (sets.count(parsed_commands[i + 1]) > 0 && sets[parsed_commands[i + 1]].expiry > std::chrono::steady_clock::now()) { // Checks it's valid and then gets
          std::string response = "$" + std::to_string(sets[parsed_commands[i + 1]].value.size()) + "\r\n" + sets[parsed_commands[i + 1]].value + "\r\n";
          send(client_fd, response.c_str(), response.size(), 0);
          break;
        }
        std::string response = "$-1\r\n";
        send(client_fd, response.c_str(), response.size(), 0);
        break;
      }

      else {
        std::string response = "+PONG\r\n";
        send(client_fd, response.c_str(), response.size(), 0);
        break;
      }
    }

    data = "";

  }
}

int main(int argc, char **argv) {
  // Flush after every std::cout / std::cerr
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;
  
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
   std::cerr << "Failed to create server socket\n";
   return 1;
  }
  
  // Since the tester restarts your program quite often, setting SO_REUSEADDR
  // ensures that we don't run into 'Address already in use' errors
  int reuse = 1;
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
    std::cerr << "setsockopt failed\n";
    return 1;
  }
  
  struct sockaddr_in server_addr;
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(6379);
  
  if (bind(server_fd, (struct sockaddr *) &server_addr, sizeof(server_addr)) != 0) {
    std::cerr << "Failed to bind to port 6379\n";
    return 1;
  }
  
  int connection_backlog = 5;
  if (listen(server_fd, connection_backlog) != 0) {
    std::cerr << "listen failed\n";
    return 1;
  }
  
  struct sockaddr_in client_addr;
  int client_addr_len = sizeof(client_addr);

  // This loop accepts clients until the server is closed
  // It creates a thread for each client and detaches them
  while (true) {
    std::cout << "Waiting for a client to connect...\n";
    int client_fd = accept(server_fd, (struct sockaddr *) &client_addr, (socklen_t *) &client_addr_len);
    if (client_fd < 0) {
      std::cerr << "Failed to accept server";
      return 1;
    }

    std::thread current_thread(thread_socket, client_fd);

    std::cout << "Client connected\n";

    current_thread.detach();

  }

  close(server_fd);

  return 0;
}
