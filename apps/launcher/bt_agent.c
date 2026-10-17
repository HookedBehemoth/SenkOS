/* The launcher's pairing agent: while pairing, yes to what BlueZ asks; else no */
#include "agent1.h"
#include "agentmanager1.h"
#include "bluetooth.h"
#include "bus.h"

static int refuse(sd_bus_message *call) {
  return sd_bus_reply_method_errorf(call, "org.bluez.Error.Rejected", "not while pairing is closed");
}
static int yes(sd_bus_message *call) { return bt_pairing_on() ? sd_bus_reply_method_return(call, NULL) : refuse(call); }

int agent1_release(sd_bus_message *call, void *userdata) {
  (void)userdata;
  return sd_bus_reply_method_return(call, NULL);
}
int agent1_cancel(sd_bus_message *call, void *userdata) {
  (void)userdata;
  return sd_bus_reply_method_return(call, NULL);
}
int agent1_request_pin_code(sd_bus_message *call, void *userdata, const char *device) {
  (void)userdata, (void)device;
  return bt_pairing_on() ? agent1_request_pin_code_reply(call, "0000") : refuse(call);
}
int agent1_request_passkey(sd_bus_message *call, void *userdata, const char *device) {
  (void)userdata, (void)device;
  return bt_pairing_on() ? agent1_request_passkey_reply(call, 0) : refuse(call);
}
int agent1_display_pin_code(sd_bus_message *call, void *userdata, const char *device, const char *pincode) {
  (void)userdata, (void)device, (void)pincode;
  return sd_bus_reply_method_return(call, NULL);
}
int agent1_display_passkey(sd_bus_message *call, void *userdata, const char *device, uint32_t passkey,
                           uint16_t entered) {
  (void)userdata, (void)device, (void)passkey, (void)entered;
  return sd_bus_reply_method_return(call, NULL);
}
int agent1_request_confirmation(sd_bus_message *call, void *userdata, const char *device, uint32_t passkey) {
  (void)userdata, (void)device, (void)passkey;
  return yes(call);
}
int agent1_request_authorization(sd_bus_message *call, void *userdata, const char *device) {
  (void)userdata, (void)device;
  return yes(call);
}
int agent1_authorize_service(sd_bus_message *call, void *userdata, const char *device, const char *uuid) {
  (void)userdata, (void)device, (void)uuid;
  return yes(call);
}

void bt_agent_add(sd_bus *bus) { sd_bus_add_object_vtable(bus, NULL, AGENT1_PATH, AGENT1_NAME, agent1_vtable, NULL); }

static void registered(const sd_bus_error *error, void *userdata) {
  (void)userdata;
  if (error == NULL)
    agentmanager1_call_request_default_agent(bus_get(), AGENT1_PATH, NULL);
}

void bt_agent_register(sd_bus *bus) {
  static const agentmanager1_register_agent_then then = {registered, NULL};
  agentmanager1_call_register_agent(bus, AGENT1_PATH, "NoInputNoOutput", &then);
}
