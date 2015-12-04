/**
 * C wrapper for the C++ PeerConnection code, to be go-compatible.
 */
#include "cpeerconnection.h"
#include "webrtc/base/common.h"
#include "talk/app/webrtc/peerconnectioninterface.h"
#include "talk/app/webrtc/test/fakeconstraints.h"
#include <iostream>
#include <unistd.h>
#include <future>

#define SUCCESS 0
#define FAILURE 1
#define TIMEOUT_SECS 3

using namespace std;
using namespace webrtc;

// Smaller typedefs
typedef rtc::scoped_refptr<webrtc::PeerConnectionInterface> PC;
typedef SessionDescriptionInterface* SDP;


// Peer acts as the glue between go and native code PeerConnectionInterface.
// However, it's not directly accessible from the Go side, which can only
// see what's exposed in the more pure extern "C" header file...
//
// This class also stubs libwebrtc's callback interface to be blocking,
// which allows the usage of goroutines, which is more idiomatic and easier
// for users of this library.
// The alternative would require casting Go function pointers, calling Go code
// from C code from Go code, which is less likely to be a good time..
// TODO(keroserene): More documentation...
class Peer
  : public CreateSessionDescriptionObserver,
    public PeerConnectionObserver {
 public:

  void Initialize() {
    promiseSDP = promise<SDP>();
    // Due to the different threading model, in order for PeerConnectionFactory
    // to be able to post async messages without getting blocked, we need to use
    // external signalling and worker threads.
    signal_thread = new rtc::Thread();
    worker_thread = new rtc::Thread();
    signal_thread->Start();
    worker_thread->Start();
    // TODO: DTLS
    // TODO: Make actual media constraints, with an exposed Go interface.
    auto c = new FakeConstraints();
    c->AddOptional(MediaConstraintsInterface::kEnableDtlsSrtp, "false");
    c->SetMandatoryReceiveAudio(false);
    c->SetMandatoryReceiveVideo(false);
    constraints = c;
    cout << "Peer initialized" << endl;
  }


  //
  // CreateSessionDescriptionObserver implementation
  //
  void OnSuccess(SDP desc) {
    cout << "[C] SDP created: " << desc << endl;
    promiseSDP.set_value(desc);
  }

  void OnFailure(const std::string& error) {
    cout << "failure: " << error << endl;
    promiseSDP.set_value(NULL);
  }

  //
  // PeerConnectionObserver Implementation
  // TODO: cgo hooks
  //
  void OnStateChange(PeerConnectionObserver::StateType state) {
    cout << "OnStateChange" << endl;
  }

  void OnAddStream(webrtc::MediaStreamInterface* stream) {
    cout << "OnAddStream" << endl;
  }

  void OnRemoveStream(webrtc::MediaStreamInterface* stream) {
    cout << "OnRemoveStream" << endl;
  }

  void OnRenegotiationNeeded() {
    cout << "OnRenegotiationNeeded" << endl;
  }

  void OnIceCandidate(const IceCandidateInterface* candidate) {
    cout << "OnIceCandidate" << candidate << endl;
  }

  void OnDataChannel(DataChannelInterface* data_channel) {
    cout << "OnDataChannel" << endl;
  }

  PeerConnectionInterface::RTCConfiguration *config;
  PeerConnectionInterface::RTCOfferAnswerOptions options;
  const MediaConstraintsInterface* constraints;

  PC pc_;

  // Passing SDPs through promises instead of callbacks.
  future<SDP> OnSDP;
  promise<SDP> promiseSDP;

  rtc::scoped_refptr<PeerConnectionFactoryInterface> pc_factory;
  // TODO: prepare and expose IceServers for real.
  PeerConnectionInterface::IceServers ice_servers;

  rtc::Thread *signal_thread;
  rtc::Thread *worker_thread;

};  // class Peer
rtc::scoped_refptr<Peer> peer;


//
// extern "C" Go-accessible functions.
//

// Expected this to be within the separate signalling thread, so nothing
// disappears.
void Initialize() {
  peer = new rtc::RefCountedObject<Peer>();
  peer->Initialize();
}


// Create and return a PeerConnection object.
// This cannot be a method in |Peer|, because this must be accessible to cgo.
CGOPeer NewPeerConnection() {
  peer->pc_factory = CreatePeerConnectionFactory(
    peer->signal_thread,
    peer->worker_thread,
    NULL, NULL, NULL);
  if (!peer->pc_factory.get()) {
    cout << "ERROR: Could not create PeerConnectionFactory" << endl;
    return NULL;
  }
  // PortAllocatorFactoryInterface *allocator;

  PeerConnectionInterface::IceServer *server = new
      PeerConnectionInterface::IceServer();
  server->uri = "stun:stun.l.google.com:19302";
  peer->ice_servers.push_back(*server);

  // Prepare RTC Configuration object. This is just the default one, for now.
  // TODO: A Go struct that can be passed and converted here.
  peer->config = new PeerConnectionInterface::RTCConfiguration();
  peer->config->servers = peer->ice_servers;

  // Prepare a native PeerConnection object.
  peer->pc_ = peer->pc_factory->CreatePeerConnection(
    *peer->config,
    peer->constraints,
    NULL,  // port allocator
    NULL,  // TODO: DTLS
    peer
    );
  if (!peer->pc_.get()) {
    cout << "ERROR: Could not create PeerConnection." << endl;
    return NULL;
  }
  cout << "[C] Made a PeerConnection: " << peer->pc_ << endl;
  return (CGOPeer)peer;
}


// PeerConnection::CreateOffer
// Blocks until libwebrtc succeeds in generating the SDP message,
// or times-out after TIMEOUT_SECS.
// @returns SDP, or NULL on failure.
CGOsdp CGOCreateOffer(CGOPeer pc) {
  // TODO: Provide an actual RTCOfferOptions as an argument.
  PC cPC = ((Peer*)pc)->pc_;
  auto r = peer->promiseSDP.get_future();
  cPC->CreateOffer(peer.get(), NULL);
  auto status = r.wait_for(chrono::seconds(TIMEOUT_SECS));
  if (future_status::ready != status) {
    cout << "[C] CreateOffer timed out after " << TIMEOUT_SECS
         << " seconds. status=" << (int)status << endl;
    return NULL;
  }
  SDP sdp = r.get();  // blocking
  // Serialize SDP message and pass string to Go.
  string *s = new string();
  sdp->ToString(s);
  cout << "sdp string: " << *s << endl;
  return (CGOsdp)s;
}

// TODO: Make this correct.
int CGOCreateAnswer(CGOPeer pc) {
  PC cPC = ((Peer*)pc)->pc_;
  cout << "[C] CreateAnswer" << peer << endl;

  cPC->CreateAnswer(peer, peer->constraints);

  // TODO: Up in PeerConnectionFactory, should probably use custom threads in
  // order for the callbacks to be *actually* registered correctly.
  cout << "[C] CreateAnswer done!" << endl;
  return SUCCESS;
}
