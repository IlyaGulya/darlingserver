#pragma once

#include <string>

namespace DarlingServer::TestDiagnostics {

bool enabled();
bool consumeFault(const char* name);
void traceLine(const std::string& line);
void traceSemaphoreCallBegin(
	const char* operation,
	int pid,
	int tid,
	unsigned int waitName,
	bool hasSignalName,
	unsigned int signalName,
	unsigned int sec,
	unsigned int nsec
);
void traceSemaphoreCallReply(
	const char* operation,
	int pid,
	int tid,
	unsigned int waitName,
	bool hasSignalName,
	unsigned int signalName,
	int resultCode
);

}
