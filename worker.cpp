#include <iostream>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <unistd.h>
#include <string>
#include <cstdlib>
#include <errno.h>
#include "resources.h"
#include <random>

using namespace std;

struct MessageBuffer {
    long mtype;
    int process_running; // 1 if running, 0 if not
};

random_device rd;
mt19937 gen(rd());

int get_resource_request(int* held_resources) {
    uniform_int_distribution<> dis(0, MAX_RESOURCES - 1);
    int resource_index = dis(gen);
    if (held_resources[resource_index] >= MAX_INSTANCES) {
        // already holding max instances of this resource, try again
        return get_resource_request(held_resources);
    }
    return resource_index;
}

int get_resource_release(int* held_resources) {
    uniform_int_distribution<> dis(0, MAX_RESOURCES - 1);
    int resource_index = dis(gen);
    if (held_resources[resource_index] == 0) {
        // not holding any instances of this resource, try again
        return get_resource_release(held_resources);
    }
    return resource_index;
}


int main(int argc, char* argv[]) {
    key_t sh_key = ftok("oss.cpp", 0);

    // create/get shared memory
    int shmid = shmget(sh_key, sizeof(int)*2, 0666);
    if (shmid == -1) {
        cerr << "shmget";
        exit(1);
    }

    // attach shared memory to shm_ptr
    int* clock = (int*) shmat(shmid, nullptr, 0);
    if (clock == (int*) -1) {
        cerr << "shmat";
        exit(1);
    }

    int *sec = &(clock[0]);
    int *nano = &(clock[1]);
    
    // get target time from command line args
    int target_seconds = stoi(argv[1]);
    int target_nano = stoi(argv[2]);

    // setup message queue
    key_t msg_key = ftok("oss.cpp", 1);
    int msgid = msgget(msg_key, 0666);
    if (msgid == -1) {
        cerr << "msgget";
        exit(1);
    }

    // how many of each resource this process has
    int held_resources[MAX_RESOURCES] = {0};
    int latest_requested_resource_index = -1;

    // Print starting message
    cout << "Worker starting, " << "PID:" << getpid() << " PPID:" << getppid() << endl
         << "Called With:" << endl
         << "Interval: " << target_seconds << " seconds, " << target_nano << " nanoseconds" << endl;

    // calculate termination time
    int end_seconds = *sec + target_seconds;
    int end_nano = *nano + target_nano;
    if (end_nano >= 1000000000) {
        end_seconds += end_nano / 1000000000;
        end_nano = end_nano % 1000000000;
    }
    
    // get random time interval for when to request/release resources
    uniform_int_distribution<> dis(0, 100000000); // between 0 and 100 milliseconds
    long long request_release_interval = dis(gen);
    long long next_request_release_total = (long long)(*sec) * 1000000000LL + (long long)(*nano) + request_release_interval;

    // setup distribution for request/release action
    uniform_int_distribution<> action_dis(1, 100);

    // worker just staring message
    cout << "Worker PID:" << getpid() << " PPID:" << getppid() << endl
         << "SysClockS: " << *sec << " SysclockNano: " << *nano << " TermTimeS: " << end_seconds << " TermTimeNano: " << end_nano << endl
         << "--Just Starting" << endl;

    // message-driven loop: block until OSS tells us to check the clock
    MessageBuffer msg;
    pid_t oss_pid = getppid();

    while (true) {
        // check if its time to terminate 
        bool should_terminate = ((*sec > end_seconds) || (*sec == end_seconds && *nano >= end_nano));

        if (should_terminate) {
            // print terminating message
            // TODO: add more deailated info
            cout << "Worker PID:" << getpid() << " PPID:" << getppid() << endl
                 << "SysClockS: " << *sec << " SysclockNano: " << *nano << " TermTimeS: " << end_seconds << " TermTimeNano: " << end_nano << endl
                 << "--Terminating" << endl;
            break; // exit loop and terminate
        }

        // check if its time to request/release resources
        long long current_total = (long long)(*sec) * 1000000000LL + (long long)(*nano);
        if (current_total >= next_request_release_total) {
            // decide whether to request or release a resource 60% request, 40% release
            int action_roll = action_dis(gen);
            if (action_roll <= 60) {
                // request resource
                int resource_index = get_resource_request(held_resources);
                // determine how much to request
                int max_amount = MAX_INSTANCES - held_resources[resource_index];
                uniform_int_distribution<> amount_dis(1, max_amount);
                int amount = amount_dis(gen);

                // out of order request
                if (resource_index <= latest_requested_resource_index) {
                    cout << "Worker PID:" << getpid() << " making out-of-order request for resource " << resource_index << endl;
                    int release_request[MAX_RESOURCES] = {0};
                    for (int i = resource_index; i < MAX_RESOURCES; ++i) {
                        if (held_resources[i] > 0) {
                            release_request[i] = held_resources[i];
                            held_resources[i] = 0;
                            cout << "Worker PID:" << getpid() << " releasing " << release_request[i] << " instances of resource " << i << " to make out-of-order request" << endl;
                        }
                    }
                }

                cout << "Worker PID:" << getpid() << " requesting " << amount << " instances of resource " << resource_index << " at SysClockS: " << *sec << " SysclockNano: " << *nano << endl;
                // update held resources
                latest_requested_resource_index = resource_index;
                held_resources[resource_index] += amount;
                next_request_release_total = (long long)(*sec) * 1000000000LL + (long long)(*nano) + request_release_interval; // schedule next request/release time
            } else {
                if (latest_requested_resource_index == -1) {
                    // no resources held, skip release
                    next_request_release_total = (long long)(*sec) * 1000000000LL + (long long)(*nano) + request_release_interval; // schedule next request/release time
                    continue;
                }
                // release resource
                // choose resource to release
                int resource_index = get_resource_release(held_resources);
                // determine how much to release
                int held_amount = held_resources[resource_index];
                uniform_int_distribution<> amount_dis(1, held_amount);
                int amount = amount_dis(gen);
                cout << "Worker PID:" << getpid() << " releasing " << amount << " instances of resource " << resource_index << " at SysClockS: " << *sec << " SysclockNano: " << *nano << endl;
                // update held resources
                held_resources[resource_index] -= amount;
                next_request_release_total = (long long)(*sec) * 1000000000LL + (long long)(*nano) + request_release_interval; // schedule next request/release time
            }
        }
    }
    shmdt(clock);
    return 0;
}