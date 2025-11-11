#include <iostream>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <unistd.h>
#include <string>
#include <cstdlib>
#include <sys/wait.h>
#include <vector>
#include <iomanip>
#include <signal.h>
#include <random>
#include <fstream>
#include <sstream>

using namespace std;

// TODO: Define total resource struct may need to be in shared memory
// TODO: Define per resource descriptor i think it just needs to say what process has what resource and how much



struct PCB {
    bool occupied;
    pid_t pid;
    int start_sec;
    int start_nano;
};

struct MessageBuffer {
    long mtype;
    int process_running; // 1 if running, 0 if not
};

// Globals
key_t sh_key = ftok("oss.cpp", 0);
int shmid = shmget(sh_key, sizeof(int)*2, IPC_CREAT | 0666);
int *shm_clock;
int *sec;
vector <PCB> table(20);
const int increment_amount = 10000;

// setup message queue
key_t msg_key = ftok("oss.cpp", 1);
int msgid = msgget(msg_key, IPC_CREAT | 0666);

void increment_clock(int* sec, int* nano, long long inc_ns) {
    const long long NSEC_PER_SEC = 1000000000LL;
    if (inc_ns <= 0) inc_ns = 1; // guard against non-positive increments
    long long total = (long long)(*nano) + inc_ns;
    *sec += (int)(total / NSEC_PER_SEC);
    *nano = (int)(total % NSEC_PER_SEC);
}

// convert float time interval to seconds and nanoseconds and return nannoseconds
int seconds_conversion(float interval) {
    int seconds = (int)interval;
    float fractional = interval - (float)seconds;
    int nanoseconds = (int)(fractional * 1e9);
    return nanoseconds;
}

// check if any child has terminated, return pid if so, else -1
pid_t child_Terminated() {
    int status;
    pid_t result = waitpid(-1, &status, WNOHANG);
    if (result > 0) {
        return result;
    }
    return -1;
}

pid_t launch_worker(float time_limit) {
    pid_t worker_pid = fork();
    if (worker_pid < 0) {
        cerr << "fork failed" << endl;
        exit(1);
    }

    if (worker_pid == 0) {
        string arg_sec = to_string((int)time_limit);
        string arg_nsec = to_string(seconds_conversion(time_limit));
        char* args[] = {
            (char*)"./worker",
            const_cast<char*>(arg_sec.c_str()),
            const_cast<char*>(arg_nsec.c_str()),
            NULL
        };
        execv(args[0], args);
        cerr << "Exec failed" << endl;
        exit(1);
    }
    return worker_pid;
}

// find an empty PCB slot, return index or -1 if none found
int find_empty_pcb(const vector<PCB> &table) {
    for (size_t i = 0; i < table.size(); ++i) {
        if (!table[i].occupied) {
            return i;
        }
    }
    return -1;
}

int remove_pcb(vector<PCB> &table, pid_t pid) {
    for (size_t i = 0; i < table.size(); ++i) {
        if (table[i].occupied && table[i].pid == pid) {
            table[i].occupied = false;
            return i;
        }
    }
    return -1;
}

void print_process_table(const std::vector<PCB> &table) {
    using std::cout;
    using std::endl;
    cout << std::left
         << std::setw(6)  << "Index"
         << std::setw(10) << "Occ"
         << std::setw(12) << "PID"
         << std::setw(12) << "StartSec"
         << std::setw(12) << "StartNano"
         << std::setw(12) << "MsgsSent" << endl;
    cout << std::string(64, '-') << endl;

    for (size_t i = 0; i < table.size(); ++i) {
        const PCB &p = table[i];
        cout << std::left << std::setw(6) << i
             << std::setw(10) << (p.occupied ? 1 : 0);
        if (p.occupied) {
            cout << std::setw(12) << p.pid
                 << std::setw(12) << p.start_sec
                 << std::setw(12) << p.start_nano;
        } else {
            cout << std::setw(12) << "-" << std::setw(12) << "-" << std::setw(12) << "-" << std::setw(12) << "-";
        }
        cout << endl;
    }
    cout << endl;
}

void signal_handler(int sig) {
    if (sig == SIGALRM || sig == SIGINT) {
        cout << "Received SIGALRM or SIGINT, terminating all child processes..." << endl;
        // Terminate all child processes and clean up shared memory
        shmdt(shm_clock);
        shmctl(shmid, IPC_RMID, nullptr);
        msgctl(msgid, IPC_RMID, nullptr);
        kill(0, SIGTERM); 
        exit(0);
    }
}

void exit_handler() {
    shmdt(shm_clock);
    shmctl(shmid, IPC_RMID, nullptr);
    msgctl(msgid, IPC_RMID, nullptr);
    exit(1);
}

// helper to detect empty/blank optarg
static inline bool optarg_blank(const char* s) {
    return (s == nullptr) || (s[0] == '\0');
}

int main(int argc, char* argv[]) {
    //parse command line args
    int proc = -1;
    int simul = -1;
    float time_limit = -1;
    float launch_interval = -1;
    string log_file = "";
    int opt;

    while((opt = getopt(argc, argv, "hn:s:t:i:f:")) != -1) {
        switch(opt) {
            case 'h': {
                cout << "Usage: oss -n proc -s simul -t time_limit -i launch_interval\n"
                    << "Options:\n"
                    << "  -h                Show this help message and exit\n"
                    << "  -n proc           Total number of worker processes to launch (non-negative integer)\n"
                    << "  -s simul          Maximum number of simultaneous worker processes (positive integer)\n"
                    << "  -t time_limit     Time limit for each worker process in seconds (non-negative float)\n"
                    << "  -i launch_interval Interval between launching worker processes in seconds (non-negative float)\n"
                    << "  -f logfile        Log file name (optional)\n"
                    << "Example:\n"
                    << "  ./oss -n 10 -s 3 -t 2.5 -i 0.5 -f oss.log\n";
                exit_handler();
            }
            case 'n': {
                if (optarg_blank(optarg)) {
                    cerr << "Error: -n requires a non-blank argument." << endl;
                    exit_handler();
                }
                try {
                    int val = stoi(optarg);
                    if (val < 0) throw invalid_argument("negative");
                    proc = val;
                } catch (...) {
                    cerr << "Error: -n must be a non-negative integer." << endl;
                    exit_handler();
                }
                 break;
            }
            case 's': {
                if (optarg_blank(optarg)) {
                    cerr << "Error: -s requires a non-blank argument." << endl;
                    exit_handler();
                }
                try {
                    int val = stoi(optarg);
                    if (val <= 0) throw invalid_argument("non-positive");
                    simul = val;
                } catch (...) {
                    cerr << "Error: -s must be a positive integer." << endl;
                    exit_handler();
                }
                 break;
            }
            case 't': {
                if (optarg_blank(optarg)) {
                    cerr << "Error: -t requires a non-blank argument." << endl;
                    exit_handler();
                }
                try {
                    float val = stof(optarg);
                    if (val < 0.0f) throw invalid_argument("negative");
                    time_limit = val;
                } catch (...) {
                    cerr << "Error: -t must be a non-negative number." << endl;
                    exit_handler();
                }
                 break;
            }
            case 'i': {
                if (optarg_blank(optarg)) {
                    cerr << "Error: -i requires a non-blank argument." << endl;
                    exit_handler();
                }
                try {
                    float val = stof(optarg);
                    if (val < 0.0f) throw invalid_argument("negative");
                    launch_interval = val;
                } catch (...) {
                    cerr << "Error: -i must be a non-negative number." << endl;
                    exit_handler();
                }
                 break;
            }
            case 'f': {
                // Optional: handle log file name if needed
                if (!optarg_blank(optarg)) log_file = optarg;
                else {
                    cerr << "Error: -f requires a non-blank filename." << endl;
                    exit_handler();
                }
                 break;
            }
            default:
                cerr << "Error: Unknown option or missing argument." << endl;
                exit_handler();
        }
    }

    // final validation of required options
    if (proc == -1 || simul == -1 || time_limit < 0.0f || launch_interval < 0.0f) {
        cerr << "Error: Missing required options. Usage: ./oss -n proc -s simul -t time_limit -i launch_interval [-f logfile]" << endl;
        exit_handler();
    }

    // attach shared memory to shm_ptr
    shm_clock = (int*) shmat(shmid, nullptr, 0);
    if (shm_clock == (int*) -1) {
        cerr << "shmat";
        exit_handler();
    }

    // pointers to seconds and nanoseconds in shared memory
    int *sec = &(shm_clock[0]);
    int *nano = &(shm_clock[1]);
    *sec = *nano = 0;

    // print interval using simulated clock: 0.5 seconds
    const long long NSEC_PER_SEC = 1000000000LL;
    const long long PRINT_INTERVAL_NANO = 500000000LL;
    long long next_print_total = (long long)(*sec) * NSEC_PER_SEC + (long long)(*nano) + PRINT_INTERVAL_NANO;

    // signal handling
    signal(SIGALRM, signal_handler);
    signal(SIGINT, signal_handler);
    alarm(60);

    // Initialize random number generator
    random_device rd;
    mt19937 gen(rd());
    uniform_real_distribution<double> dis(1, time_limit);

    // open log file if specified
    ofstream log_fs;
    if (!log_file.empty()) {
        log_fs.open(log_file);
        if (!log_fs) {
            cerr << "Error: Could not open log file " << log_file << endl;
            exit(1);
        }
    }

    // helper to log messages originating from OSS (writes to stdout and to log file if open)
    auto oss_log = [&](const string &s) {
        cout << s;
        if (log_fs.is_open()) log_fs << s;
    };

    // oss starting message
    {
        ostringstream ss;
        ss << "OSS starting, PID:" << getpid() << " PPID:" << getppid() << endl
           << "Called With:" << endl
           << "-n: " << proc << endl
           << "-s: " << simul << endl
           << "-t: " << time_limit << endl
           << "-i: " << launch_interval << endl;
        oss_log(ss.str());
    }
 
    int launched_processes = 0;
    int running_processes = 0;

    long long launch_interval_nano = (long long)(launch_interval * 1e9); // convert launch interval to nanoseconds
    long long next_launch_total = 0; 

    MessageBuffer sndMessage;

    while (launched_processes < proc || running_processes > 0) {
        increment_clock(sec, nano, increment_amount);

        // Check if it's time to launch a new worker
        long long current_total = (long long)(*sec) * NSEC_PER_SEC + (long long)(*nano);
        if (launched_processes < proc && running_processes < simul && current_total >= next_launch_total) {
            pid_t worker_pid = launch_worker(time_limit);

            // Find empty slot in PCB array and populate it with new process info
            int pcb_index = find_empty_pcb(table);
            table[pcb_index].occupied = true;
            table[pcb_index].pid = worker_pid;
            table[pcb_index].start_sec = *sec;
            table[pcb_index].start_nano = *nano;

            launched_processes++;
            running_processes++;

            // Update the next allowed launch time
            next_launch_total = current_total + launch_interval_nano;
            print_process_table(table);
        }

        // call print_process_table every half-second of simulated time
        {
            long long current_total = (long long)(*sec) * NSEC_PER_SEC + (long long)(*nano);
            while (current_total >= next_print_total) {
                print_process_table(table);
                next_print_total += PRINT_INTERVAL_NANO;
            }
        }

        pid_t terminated_pid = child_Terminated();
        if (terminated_pid > 0) {
            remove_pcb(table, terminated_pid);
            running_processes--;
            cout << "OSS: Detected termination of child PID " << terminated_pid << endl;
        }


    }

    // cleanup
     shmdt(shm_clock);
     shmctl(shmid, IPC_RMID, nullptr);
     msgctl(msgid, IPC_RMID, nullptr);
     return 0;
 }