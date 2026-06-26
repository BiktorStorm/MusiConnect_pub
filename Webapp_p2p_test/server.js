// =============================================================================
// SIGNALING SERVER
// =============================================================================
//
// PURPOSE:
// WebRTC peers can't connect directly without first exchanging connection info.
// This server is the "matchmaker" — it relays setup messages between two browsers
// so they can establish a direct P2P connection.
//
// WHAT IT RELAYS:
// 1. "offer"     — Peer A says "here's what audio I can send, here's how to reach me"
// 2. "answer"    — Peer B says "got it, here's my info back"
// 3. "candidate" — Both peers share network path candidates (IP:port combos to try)
//
// AFTER THE HANDSHAKE:
// Audio flows DIRECTLY between the browsers (not through this server).
// This server can even be shut down once peers are connected.
//
// WHY WEBSOCKETS:
// Both peers need to receive messages they didn't ask for (push, not poll).
// WebSocket gives us persistent bidirectional communication — perfect for signaling.
// =============================================================================

const BUN = typeof Bun !== 'undefined';

// Track connected peers. For this demo: exactly 2 peers in one "room".
const peers = new Set();

const server = Bun.serve({
  port: 3000,
  
  // Serve the HTML client page on regular HTTP requests
  fetch(req, server) {
    const url = new URL(req.url);
    
    // Upgrade WebSocket requests
    if (url.pathname === '/ws') {
      if (server.upgrade(req)) return;
      return new Response('WebSocket upgrade failed', { status: 500 });
    }
    
    // Serve the client HTML file for everything else
    return new Response(Bun.file('./client.html'), {
      headers: { 'Content-Type': 'text/html' }
    });
  },
  
  websocket: {
    open(ws) {
      peers.add(ws);
      console.log(`Peer connected (${peers.size}/2)`);
      
      // Tell the client how many peers are connected
      ws.send(JSON.stringify({ type: 'peer-count', count: peers.size }));
      
      // If this is the second peer, tell the first peer to start the offer
      // (WebRTC requires one side to initiate — we pick whoever connected first)
      if (peers.size === 2) {
        for (const peer of peers) {
          if (peer !== ws) {
            peer.send(JSON.stringify({ type: 'start-call' }));
          }
        }
      }
    },
    
    message(ws, message) {
      // Relay signaling messages to the OTHER peer.
      // We don't inspect or modify them — just forward.
      for (const peer of peers) {
        if (peer !== ws) {
          peer.send(message);
        }
      }
    },
    
    close(ws) {
      peers.delete(ws);
      console.log(`Peer disconnected (${peers.size}/2)`);
    }
  }
});

console.log(`Signaling server running at http://localhost:${server.port}`);
console.log('Open this URL in two browser tabs to test P2P audio.');
