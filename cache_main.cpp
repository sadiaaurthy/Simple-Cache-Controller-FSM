#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace std;

static constexpr uint32_t WORD_SIZE_BYTES = 4;
static constexpr uint32_t BLOCK_SIZE_BYTES = 16;          // 4 words
static constexpr uint32_t CACHE_LINES = 1024;             // 16 KiB / 16 B
static constexpr uint32_t INDEX_BITS = 10;
static constexpr uint32_t OFFSET_BITS = 4;
static constexpr uint32_t TAG_BITS = 18;
static constexpr int MEMORY_READ_LATENCY = 2;
static constexpr int MEMORY_WRITE_LATENCY = 2;

enum class OpType { Load, Store };
enum class FSMState { Idle, CompareTag, WriteBack, Allocate };

struct Request {
    OpType op;
    string reg;
    uint32_t address;
    string rawLine;
    int lineNumber;
};

struct CacheLine {
    bool valid = false;
    bool dirty = false;
    uint32_t tag = 0;
    uint32_t words[4] = {0, 0, 0, 0};
};

struct ParsedAddress {
    uint32_t tag;
    uint32_t index;
    uint32_t blockOffset;
    uint32_t wordOffset;
};

struct Signals {
    int cpuReadWrite = 0;      // 0 = read, 1 = write
    int cpuValid = 0;
    uint32_t cpuAddress = 0;
    uint32_t cpuWriteData = 0;
    uint32_t cpuReadData = 0;
    int cpuReady = 0;

    int memReadWrite = 0;      // 0 = read, 1 = write
    int memValid = 0;
    uint32_t memAddress = 0;
    uint32_t memWriteBlock[4] = {0, 0, 0, 0};
    uint32_t memReadBlock[4] = {0, 0, 0, 0};
    int memReady = 0;
};

class SimpleMemory {
public:
    SimpleMemory() = default;

    uint32_t loadWord(uint32_t address) const {
        auto it = memoryWords.find(address);
        if (it != memoryWords.end()) {
            return it->second;
        }
        return defaultWordValue(address);
    }

    void storeWord(uint32_t address, uint32_t value) {
        memoryWords[address] = value;
    }

    void readBlock(uint32_t blockBase, uint32_t outWords[4]) const {
        for (int i = 0; i < 4; ++i) {
            outWords[i] = loadWord(blockBase + i * WORD_SIZE_BYTES);
        }
    }

    void writeBlock(uint32_t blockBase, const uint32_t inWords[4]) {
        for (int i = 0; i < 4; ++i) {
            storeWord(blockBase + i * WORD_SIZE_BYTES, inWords[i]);
        }
    }

private:
    unordered_map<uint32_t, uint32_t> memoryWords;

    static uint32_t defaultWordValue(uint32_t address) {
        return 0xA5000000u | ((address >> 2) & 0x00FFFFFFu);
    }
};


static string trimText(const string& s) {
    const auto start = s.find_first_not_of(" \t\r\n");
    if (start == string::npos) return "";
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static string upperText(string s) {
    for (char& c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return s;
}

class Simulator {
public:
    explicit Simulator(vector<Request> requests)
        : requests_(std::move(requests)) {
        initializeRegisters();
    }

    void run(ostream& out) {
        out << "Simple Cache Controller FSM Simulation\n";
        out << "====================================\n";
        out << "Cache organization: direct-mapped, write-back, write allocate\n";
        out << "Block size: 16 bytes (4 words), Cache lines: 1024, Address width: 32 bits\n";
        out << "Address fields -> Tag: 18 bits, Index: 10 bits, Block offset: 4 bits\n";
        out << "Memory latency -> Read: " << MEMORY_READ_LATENCY
            << " cycles, Write: " << MEMORY_WRITE_LATENCY << " cycles\n\n";

        for (size_t i = 0; i < requests_.size(); ++i) {
            currentRequest_ = requests_[i];
            processRequest(out, static_cast<int>(i + 1));
        }

        out << "Simulation completed in " << globalCycle_ << " cycles.\n";
    }

private:
    vector<Request> requests_;
    vector<CacheLine> cache_{CACHE_LINES};
    SimpleMemory memory_;
    unordered_map<string, uint32_t> registers_;
    Signals signals_{};
    optional<Request> currentRequest_;
    ParsedAddress currentAddr_{};
    FSMState state_ = FSMState::Idle;
    int globalCycle_ = 0;
    int memoryCountdown_ = 0;
    string pendingMemoryAction_;
    uint32_t pendingMemAddress_ = 0;
    uint32_t pendingReadBlock_[4] = {0, 0, 0, 0};

    static string trim(const string& s) {
        const auto start = s.find_first_not_of(" \t\r\n");
        if (start == string::npos) return "";
        const auto end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    static string toUpperCopy(string s) {
        for (char& c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        return s;
    }

    static string stateName(FSMState s) {
        switch (s) {
            case FSMState::Idle: return "Idle";
            case FSMState::CompareTag: return "Compare Tag";
            case FSMState::WriteBack: return "Write-Back";
            case FSMState::Allocate: return "Allocate";
        }
        return "Unknown";
    }

    static string opName(OpType op) {
        return (op == OpType::Load) ? "lw" : "sw";
    }

    void initializeRegisters() {
        for (int i = 0; i < 32; ++i) {
            ostringstream name;
            name << 'x' << i;
            registers_[name.str()] = 0x1000u + static_cast<uint32_t>(i) * 0x11111111u;
        }
    }

    static ParsedAddress decode(uint32_t address) {
        ParsedAddress pa{};
        pa.blockOffset = address & ((1u << OFFSET_BITS) - 1u);
        pa.wordOffset = (pa.blockOffset >> 2) & 0x3u;
        pa.index = (address >> OFFSET_BITS) & ((1u << INDEX_BITS) - 1u);
        pa.tag = address >> (INDEX_BITS + OFFSET_BITS);
        return pa;
    }

    static uint32_t blockBaseAddress(uint32_t address) {
        return address & ~(BLOCK_SIZE_BYTES - 1u);
    }

    static uint32_t composeBlockAddress(uint32_t tag, uint32_t index) {
        return (tag << (INDEX_BITS + OFFSET_BITS)) | (index << OFFSET_BITS);
    }

    void resetSignals() {
        signals_ = Signals{};
    }

    void logCycle(ostream& out, const string& actionNote) {
        out << "Cycle " << setw(3) << globalCycle_ << " | "
            << left << setw(11) << stateName(state_) << right
            << " | " << actionNote << "\n";
        out << "        CPU: valid=" << signals_.cpuValid
            << " rw=" << signals_.cpuReadWrite
            << " addr=0x" << hex << setw(8) << setfill('0') << signals_.cpuAddress
            << " wdata=0x" << setw(8) << signals_.cpuWriteData
            << " rdata=0x" << setw(8) << signals_.cpuReadData
            << dec << setfill(' ') << " ready=" << signals_.cpuReady << "\n";
        out << "        MEM: valid=" << signals_.memValid
            << " rw=" << signals_.memReadWrite
            << " addr=0x" << hex << setw(8) << setfill('0') << signals_.memAddress
            << dec << setfill(' ') << " ready=" << signals_.memReady;
        if (signals_.memValid) {
            out << " block=[";
            for (int i = 0; i < 4; ++i) {
                if (i) out << ", ";
                out << "0x" << hex << setw(8) << setfill('0')
                    << (signals_.memReadWrite ? signals_.memWriteBlock[i] : signals_.memReadBlock[i]);
            }
            out << dec << setfill(' ') << "]";
        }
        out << "\n";
    }

    void startMemoryRead(uint32_t addr) {
        pendingMemoryAction_ = "READ";
        pendingMemAddress_ = addr;
        memoryCountdown_ = MEMORY_READ_LATENCY;
        memory_.readBlock(addr, pendingReadBlock_);
    }

    void startMemoryWrite(uint32_t addr, const uint32_t words[4]) {
        pendingMemoryAction_ = "WRITE";
        pendingMemAddress_ = addr;
        memoryCountdown_ = MEMORY_WRITE_LATENCY;
        for (int i = 0; i < 4; ++i) {
            pendingReadBlock_[i] = words[i];
        }
    }

    bool stepMemory() {
        if (memoryCountdown_ <= 0) return false;
        --memoryCountdown_;
        if (memoryCountdown_ == 0) {
            if (pendingMemoryAction_ == "WRITE") {
                memory_.writeBlock(pendingMemAddress_, pendingReadBlock_);
            }
            return true;
        }
        return false;
    }

    void processRequest(ostream& out, int requestNumber) {
        const Request& req = *currentRequest_;
        currentAddr_ = decode(req.address);
        out << "------------------------------------------------------------\n";
        out << "Request " << requestNumber << ": " << req.rawLine << "\n";
        out << "Decoded address -> tag=0x" << hex << currentAddr_.tag
            << ", index=0x" << currentAddr_.index
            << ", block offset=0x" << currentAddr_.blockOffset
            << ", word offset=" << dec << currentAddr_.wordOffset << "\n";
        out << "------------------------------------------------------------\n";

        state_ = FSMState::Idle;
        bool completed = false;

        while (!completed) {
            ++globalCycle_;
            resetSignals();
            signals_.cpuValid = 1;
            signals_.cpuReadWrite = (req.op == OpType::Store) ? 1 : 0;
            signals_.cpuAddress = req.address;
            signals_.cpuWriteData = (req.op == OpType::Store) ? registers_[req.reg] : 0;

            CacheLine& line = cache_[currentAddr_.index];
            bool memFinishedThisCycle = stepMemory();

            switch (state_) {
                case FSMState::Idle: {
                    state_ = FSMState::CompareTag;
                    logCycle(out, "Valid CPU request moves FSM from Idle to Compare Tag.");
                    break;
                }
                case FSMState::CompareTag: {
                    bool hit = line.valid && line.tag == currentAddr_.tag;
                    if (hit) {
                        if (req.op == OpType::Load) {
                            signals_.cpuReadData = line.words[currentAddr_.wordOffset];
                            registers_[req.reg] = signals_.cpuReadData;
                        } else {
                            line.words[currentAddr_.wordOffset] = registers_[req.reg];
                            line.dirty = true;
                            line.valid = true;
                            line.tag = currentAddr_.tag;
                        }
                        signals_.cpuReady = 1;
                        logCycle(out, hitNote(req, line));
                        state_ = FSMState::Idle;
                        completed = true;
                    } else {
                        if (line.valid && line.dirty) {
                            uint32_t wbAddress = composeBlockAddress(line.tag, currentAddr_.index);
                            signals_.memValid = 1;
                            signals_.memReadWrite = 1;
                            signals_.memAddress = wbAddress;
                            for (int i = 0; i < 4; ++i) signals_.memWriteBlock[i] = line.words[i];
                            if (memoryCountdown_ == 0) startMemoryWrite(wbAddress, line.words);
                            logCycle(out, "Cache miss and old block is dirty, so the FSM goes to Write-Back.");
                            state_ = FSMState::WriteBack;
                        } else {
                            uint32_t allocAddress = blockBaseAddress(req.address);
                            signals_.memValid = 1;
                            signals_.memReadWrite = 0;
                            signals_.memAddress = allocAddress;
                            for (int i = 0; i < 4; ++i) signals_.memReadBlock[i] = pendingReadBlock_[i];
                            if (memoryCountdown_ == 0) startMemoryRead(allocAddress);
                            logCycle(out, "Cache miss and old block is clean, so the FSM goes to Allocate.");
                            state_ = FSMState::Allocate;
                        }
                    }
                    break;
                }
                case FSMState::WriteBack: {
                    uint32_t wbAddress = pendingMemAddress_;
                    signals_.memValid = 1;
                    signals_.memReadWrite = 1;
                    signals_.memAddress = wbAddress;
                    for (int i = 0; i < 4; ++i) signals_.memWriteBlock[i] = pendingReadBlock_[i];
                    signals_.memReady = memFinishedThisCycle ? 1 : 0;

                    if (memFinishedThisCycle) {
                        line.dirty = false;
                        uint32_t allocAddress = blockBaseAddress(req.address);
                        startMemoryRead(allocAddress);
                        signals_.memValid = 1;
                        signals_.memReadWrite = 0;
                        signals_.memAddress = allocAddress;
                        logCycle(out, "Ready signal from memory completes the write; FSM goes to Allocate.");
                        state_ = FSMState::Allocate;
                    } else {
                        logCycle(out, "Write-Back waits because memory is not ready.");
                    }
                    break;
                }
                case FSMState::Allocate: {
                    signals_.memValid = 1;
                    signals_.memReadWrite = 0;
                    signals_.memAddress = pendingMemAddress_;
                    for (int i = 0; i < 4; ++i) signals_.memReadBlock[i] = pendingReadBlock_[i];
                    signals_.memReady = memFinishedThisCycle ? 1 : 0;

                    if (memFinishedThisCycle) {
                        line.valid = true;
                        line.dirty = false;
                        line.tag = currentAddr_.tag;
                        for (int i = 0; i < 4; ++i) line.words[i] = pendingReadBlock_[i];
                        logCycle(out, "Memory read is complete; FSM returns to Compare Tag to finish the request.");
                        state_ = FSMState::CompareTag;
                    } else {
                        logCycle(out, "Allocate waits for memory to return the new block.");
                    }
                    break;
                }
            }
        }

        out << "Final cache line status at index " << currentAddr_.index << ": valid="
            << cache_[currentAddr_.index].valid << ", dirty=" << cache_[currentAddr_.index].dirty
            << ", tag=0x" << hex << cache_[currentAddr_.index].tag << dec << "\n\n";
    }

    string hitNote(const Request& req, const CacheLine&) const {
        if (req.op == OpType::Load) {
            ostringstream oss;
            oss << "Cache hit. Data are read from the selected word and the Ready signal is sent.";
            return oss.str();
        }
        ostringstream oss;
        oss << "Cache hit. Data are written to the selected word, dirty bit is set, and the Ready signal is sent.";
        return oss.str();
    }
};

uint32_t parseNumber(const string& token) {
    size_t idx = 0;
    uint32_t value = static_cast<uint32_t>(stoul(token, &idx, 0));
    if (idx != token.size()) {
        throw invalid_argument("Invalid number: " + token);
    }
    return value;
}

vector<Request> loadRequests(const string& path) {
    ifstream in(path);
    if (!in) {
        throw runtime_error("Could not open input file: " + path);
    }

    vector<Request> requests;
    string line;
    int lineNo = 0;
    while (getline(in, line)) {
        ++lineNo;
        string original = line;
        auto commentPos = line.find('#');
        if (commentPos != string::npos) line = line.substr(0, commentPos);
        line = trimText(line);
        if (line.empty()) continue;

        for (char& c : line) {
            if (c == ',') c = ' ';
        }
        istringstream iss(line);
        string op, reg, addrToken;
        if (!(iss >> op >> reg >> addrToken)) {
            throw runtime_error("Parse error on line " + to_string(lineNo) + ": " + original);
        }
        op = upperText(op);
        Request req{};
        if (op == "LW") {
            req.op = OpType::Load;
        } else if (op == "SW") {
            req.op = OpType::Store;
        } else {
            throw runtime_error("Unsupported operation on line " + to_string(lineNo) + ": " + op);
        }
        req.reg = reg;
        req.address = parseNumber(addrToken);
        req.rawLine = trimText(original);
        req.lineNumber = lineNo;
        requests.push_back(req);
    }

    if (requests.empty()) {
        throw runtime_error("No valid requests were found in the input file.");
    }
    return requests;
}

int main(int argc, char* argv[]) {
    try {
        string inputPath = "instructions.txt";
        string outputPath = "output.txt";
        if (argc >= 2) inputPath = argv[1];
        if (argc >= 3) outputPath = argv[2];

        vector<Request> requests = loadRequests(inputPath);
        ofstream out(outputPath);
        if (!out) {
            throw runtime_error("Could not open output file: " + outputPath);
        }

        Simulator simulator(requests);
        simulator.run(out);

        cout << "Simulation complete. Trace written to: " << outputPath << "\n";
        return 0;
    } catch (const exception& ex) {
        cerr << "Error: " << ex.what() << "\n";
        return 1;
    }
}
