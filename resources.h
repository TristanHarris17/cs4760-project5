#ifndef RESOURCES_H
#define RESOURCES_H

#define MAX_RESOURCES 10
#define MAX_INSTANCES 5
#define MAX_PROCESSES 18

struct resource_descriptor {
    const int total_resources[MAX_RESOURCES] = {5, 5, 5, 5, 5, 5, 5, 5, 5, 5};
    int available_resources[MAX_RESOURCES] = {5, 5, 5, 5, 5, 5, 5, 5, 5, 5};
    int allocated_resources[MAX_PROCESSES][MAX_RESOURCES] = {0}; // slot for each possible process slot that the process gets in PCB will correspond to this
    int requested_resources[MAX_PROCESSES][MAX_RESOURCES] = {0};
};

#endif