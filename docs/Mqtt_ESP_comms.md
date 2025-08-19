# MQTT communication ESP

## Communication to ESP

### topic: cmd
Sends a command to the ESP for execution. This should also include command to request as a feedback the list of implemented commands (help-like).
- Mqtt to ESP: command and its parameters
- ESP to MQTT: Feedback
### topic: cmd/ota

### topic: info

- ESP to MQTT:
  - periodic status info which are usually not stored (unless the server interface is requred to do so)
  - ping (might include the above)
### topic: DAT
sends data packets which need to be processed for storage in database
- Mqtt to ESP: none. request to start/stop a flow should come through command


## implementation

using a 