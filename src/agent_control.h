#ifndef FALLOUT_AGENT_CONTROL_H_
#define FALLOUT_AGENT_CONTROL_H_

namespace fallout {

bool agentControlConfigure(int argc, char** argv);
bool agentControlEnabled();
const char* agentControlPath();
void agentControlStart();
void agentControlStop();
void agentControlClose();

} // namespace fallout

#endif /* FALLOUT_AGENT_CONTROL_H_ */
