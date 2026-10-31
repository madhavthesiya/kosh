#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include "InMemoryStorage.h"
#include "RedisLite.h"
#include "Commands.h"

std::vector<std::string> split(const std::string& input) {
    std::vector<std::string> tokens;
    std::istringstream stream(input);
    std::string token;
    while(stream >> token) tokens.push_back(token);
    return tokens;
}

std::string toUpper(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(), ::toupper);
    return result;
}

void printResult(const Result& r) {
    if(r.success) {
        if(!r.values.empty()) {
            for(size_t i = 0; i < r.values.size(); i++) {
                std::cout << (i+1) << ") " << r.values[i] << "\n";
            }
            if(r.values.empty()) std::cout << "(empty list or set)\n";
        } else if(!r.value.empty()) {
            std::cout << r.value << "\n";
        } else {
            std::cout << "OK\n";
        }
    } else {
        switch(r.error) {
            case ErrorCode::KEY_NOT_FOUND: std::cout << "(nil)\n"; break;
            case ErrorCode::INVALID_KEY:   std::cout << "ERROR: invalid key\n"; break;
            case ErrorCode::FILE_ERROR:    std::cout << "ERROR: file operation failed\n"; break;
            case ErrorCode::WRONG_TYPE:    std::cout << "WRONGTYPE Operation against a key holding the wrong kind of value\n"; break;
            default:                       std::cout << "ERROR\n"; break;
        }
    }
}

void printHelp() {
    std::cout << "\n=== Mini-Redis Commands ===\n\n";
    std::cout << "  String:  SET key value | GET key | SETEX key ttl value\n";
    std::cout << "  Key:     DEL key | EXISTS key | TYPE key | EXPIRE key secs | TTL key\n";
    std::cout << "  List:    LPUSH key val [...] | RPUSH key val [...] | LPOP key | RPOP key\n";
    std::cout << "           LRANGE key start stop | LLEN key\n";
    std::cout << "  Set:     SADD key member [...] | SREM key member [...] | SMEMBERS key\n";
    std::cout << "           SISMEMBER key member | SCARD key\n";
    std::cout << "  Hash:    HSET key field val [...] | HGET key field | HDEL key field [...]\n";
    std::cout << "           HGETALL key | HLEN key\n";
    std::cout << "  Persist: SAVE filename | LOAD filename | AOFREWRITE\n";
    std::cout << "  Other:   HELP | EXIT\n\n";
}

int main() {
    // AOF logger — every write command is appended to this file
    AOFLogger aof("appendonly.aof");
    InMemoryStorage storage(100, &aof);
    RedisLite redis(storage);
    std::string line;

    // Replay AOF on startup to restore state from previous session
    replayAOF(storage, aof);

    std::cout << "Mini-Redis v2.0 (AOF enabled) — Type HELP for commands\n";
    std::cout << "mini-redis> ";

    while(std::getline(std::cin, line)) {
        if(line.empty()) { std::cout << "mini-redis> "; continue; }
        auto tokens = split(line);
        if(tokens.empty()) { std::cout << "mini-redis> "; continue; }

        std::string cmd = toUpper(tokens[0]);

        // ==================== String Commands ====================
        if(cmd == "SET" && tokens.size() == 3) {
            printResult(redis.set(tokens[1], tokens[2]));
        }
        else if(cmd == "GET" && tokens.size() == 2) {
            printResult(redis.get(tokens[1]));
        }
        else if(cmd == "SETEX" && tokens.size() == 4) {
            try {
                int ttl = std::stoi(tokens[2]);
                printResult(redis.setex(tokens[1], ttl, tokens[3]));
            } catch(...) { std::cout << "ERROR: TTL must be an integer\n"; }
        }
        // ==================== Key Commands ====================
        else if(cmd == "DEL" && tokens.size() == 2) {
            printResult(redis.del(tokens[1]));
        }
        else if(cmd == "EXISTS" && tokens.size() == 2) {
            printResult(redis.exists(tokens[1]));
        }
        else if(cmd == "TYPE" && tokens.size() == 2) {
            printResult(redis.type(tokens[1]));
        }
        else if(cmd == "EXPIRE" && tokens.size() == 3) {
            try {
                int ttl = std::stoi(tokens[2]);
                printResult(redis.expire(tokens[1], ttl));
            } catch(...) { std::cout << "ERROR: TTL must be an integer\n"; }
        }
        else if(cmd == "TTL" && tokens.size() == 2) {
            printResult(redis.ttl(tokens[1]));
        }
        // ==================== List Commands ====================
        else if(cmd == "LPUSH" && tokens.size() >= 3) {
            std::vector<std::string> vals(tokens.begin() + 2, tokens.end());
            printResult(redis.lpush(tokens[1], vals));
        }
        else if(cmd == "RPUSH" && tokens.size() >= 3) {
            std::vector<std::string> vals(tokens.begin() + 2, tokens.end());
            printResult(redis.rpush(tokens[1], vals));
        }
        else if(cmd == "LPOP" && tokens.size() == 2) {
            printResult(redis.lpop(tokens[1]));
        }
        else if(cmd == "RPOP" && tokens.size() == 2) {
            printResult(redis.rpop(tokens[1]));
        }
        else if(cmd == "LRANGE" && tokens.size() == 4) {
            try {
                int start = std::stoi(tokens[2]);
                int stop = std::stoi(tokens[3]);
                printResult(redis.lrange(tokens[1], start, stop));
            } catch(...) { std::cout << "ERROR: start/stop must be integers\n"; }
        }
        else if(cmd == "LLEN" && tokens.size() == 2) {
            printResult(redis.llen(tokens[1]));
        }
        // ==================== Set Commands ====================
        else if(cmd == "SADD" && tokens.size() >= 3) {
            std::vector<std::string> members(tokens.begin() + 2, tokens.end());
            printResult(redis.sadd(tokens[1], members));
        }
        else if(cmd == "SREM" && tokens.size() >= 3) {
            std::vector<std::string> members(tokens.begin() + 2, tokens.end());
            printResult(redis.srem(tokens[1], members));
        }
        else if(cmd == "SMEMBERS" && tokens.size() == 2) {
            printResult(redis.smembers(tokens[1]));
        }
        else if(cmd == "SISMEMBER" && tokens.size() == 3) {
            printResult(redis.sismember(tokens[1], tokens[2]));
        }
        else if(cmd == "SCARD" && tokens.size() == 2) {
            printResult(redis.scard(tokens[1]));
        }
        // ==================== Hash Commands ====================
        else if(cmd == "HSET" && tokens.size() >= 4 && tokens.size() % 2 == 0) {
            std::vector<std::pair<std::string,std::string>> fields;
            for(size_t i = 2; i < tokens.size(); i += 2) {
                fields.emplace_back(tokens[i], tokens[i+1]);
            }
            printResult(redis.hset(tokens[1], fields));
        }
        else if(cmd == "HGET" && tokens.size() == 3) {
            printResult(redis.hget(tokens[1], tokens[2]));
        }
        else if(cmd == "HDEL" && tokens.size() >= 3) {
            std::vector<std::string> fields(tokens.begin() + 2, tokens.end());
            printResult(redis.hdel(tokens[1], fields));
        }
        else if(cmd == "HGETALL" && tokens.size() == 2) {
            printResult(redis.hgetall(tokens[1]));
        }
        else if(cmd == "HLEN" && tokens.size() == 2) {
            printResult(redis.hlen(tokens[1]));
        }
        // ==================== Persistence ====================
        else if(cmd == "SAVE" && tokens.size() == 2) {
            printResult(redis.save(tokens[1]));
        }
        else if(cmd == "LOAD" && tokens.size() == 2) {
            printResult(redis.load(tokens[1]));
        }
        else if(cmd == "AOFREWRITE") {
            auto commands = storage.dumpState();
            aof.rewrite(commands);
            std::cout << "AOF rewritten with " << commands.size() << " commands\n";
        }
        // ==================== Utility ====================
        else if(cmd == "HELP") {
            printHelp();
        }
        else if(cmd == "EXIT" || cmd == "QUIT") {
            std::cout << "Bye!\n";
            break;
        }
        else {
            std::cout << "ERROR: unknown command '" << tokens[0] << "' (type HELP for usage)\n";
        }

        std::cout << "mini-redis> ";
    }
    return 0;
}
