/* The launcher's pairing agent: while pairing, yes to what BlueZ asks; else no */
#include "agent1.h"
#include "agentmanager1.h"
#include "bluetooth.h"
#include "bus.h"

static int refuse(bus_req *req) {
  return bus_reply_error(req, S("org.bluez.Error.Rejected"), S("not while pairing is closed"));
}
static int yes(bus_req *req) { return bt_pairing_on() ? bus_reply(req) : refuse(req); }

int agent1_release(bus_req *req, void *userdata) {
  (void)userdata;
  return bus_reply(req);
}
int agent1_cancel(bus_req *req, void *userdata) {
  (void)userdata;
  return bus_reply(req);
}
int agent1_request_pin_code(bus_req *req, void *userdata, str device) {
  (void)userdata, (void)device;
  return bt_pairing_on() ? agent1_request_pin_code_reply(req, S("0000")) : refuse(req);
}
int agent1_request_passkey(bus_req *req, void *userdata, str device) {
  (void)userdata, (void)device;
  return bt_pairing_on() ? agent1_request_passkey_reply(req, 0) : refuse(req);
}
int agent1_display_pin_code(bus_req *req, void *userdata, str device, str pincode) {
  (void)userdata, (void)device, (void)pincode;
  return bus_reply(req);
}
int agent1_display_passkey(bus_req *req, void *userdata, str device, uint32_t passkey,
                           uint16_t entered) {
  (void)userdata, (void)device, (void)passkey, (void)entered;
  return bus_reply(req);
}
int agent1_request_confirmation(bus_req *req, void *userdata, str device, uint32_t passkey) {
  (void)userdata, (void)device, (void)passkey;
  return yes(req);
}
int agent1_request_authorization(bus_req *req, void *userdata, str device) {
  (void)userdata, (void)device;
  return yes(req);
}
int agent1_authorize_service(bus_req *req, void *userdata, str device, str uuid) {
  (void)userdata, (void)device, (void)uuid;
  return yes(req);
}

void bt_agent_add(bus_conn *bus) { agent1_serve(bus, NULL); }

static bus_call registering;

/* Registered: be the default one */
static void registered(bus_call *call, const bus_error *error) {
  (void)call;
  if (error == NULL)
    agentmanager1_call_request_default_agent(bus_get(), S(AGENT1_PATH), NULL, NULL);
}

void bt_agent_register(bus_conn *bus) {
  agentmanager1_call_register_agent(bus, S(AGENT1_PATH), S("NoInputNoOutput"), &registering, registered);
}
