# jsbeeb bridge setup

Step-by-step setup for the [jsbeeb bridge](../README.md#jsbeeb-bridge)
described in the README. Once both sides are running, press `J` in
the C++ port to push state, then type in jsbeeb to drive the port's
player.

## The two sides

1. **The C++ port** runs an embedded HTTP server on
   `http://localhost:5173`. SSE events at `/bridge/events` push pokes
   into jsbeeb; POSTs to `/bridge/input` read keypresses back. No
   external dependencies — the server starts the first time you press
   `J`.

2. **The jsbeeb side** needs a small client that connects to that SSE
   stream and POSTs the BBC's `action_keys_pressed` table back each
   frame. Paste the snippet below into jsbeeb's DevTools console
   (Chrome: `Ctrl+Shift+J`) after booting Exile in jsbeeb:

```javascript
const es=new EventSource('http://localhost:5173/bridge/events');
es.onmessage=e=>{const m=JSON.parse(e.data);if(m.type==='poke')m.writes.forEach(w=>processor.writemem(w.addr,w.value));};
const B=0x126b,N=0x27,U='http://localhost:5173/bridge/input',L=Array(N).fill(-1);
(function loop(){const a=Array(N);let d=false;for(let i=0;i<N;i++){const b=processor.readmem(B+i)&0x80?1:0;a[i]=b;if(b!==L[i])d=true;}if(d){L.splice(0,N,...a);fetch(U,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({actions:a}),keepalive:true}).catch(()=>{});}requestAnimationFrame(loop);})();
```

Once the client is running, press `J` in the C++ port to fire a state
push. Typing in jsbeeb afterwards drives the C++ port's player too —
the input loop OR-merges jsbeeb's `action_keys_pressed` into our
`InputState` each frame.

## Bookmarklet

To avoid pasting the snippet every time, save it as a browser
bookmarklet. Create a new bookmark in Chrome (any name) and use this
as the URL:

```
javascript:(function(){const es=new EventSource('http://localhost:5173/bridge/events');es.onmessage=e=>{const m=JSON.parse(e.data);if(m.type==='poke')m.writes.forEach(w=>processor.writemem(w.addr,w.value));};const B=0x126b,N=0x27,U='http://localhost:5173/bridge/input',L=Array(N).fill(-1);(function loop(){const a=Array(N);let d=false;for(let i=0;i<N;i++){const b=processor.readmem(B+i)&0x80?1:0;a[i]=b;if(b!==L[i])d=true;}if(d){L.splice(0,N,...a);fetch(U,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({actions:a}),keepalive:true}).catch(()=>{});}requestAnimationFrame(loop);})();})();
```

With jsbeeb's tab focused, click the bookmark to start the bridge
client. Then press `J` in the port to push state.

The CORS preflight from the port includes
`Access-Control-Allow-Private-Network: true` so Chrome 102+ permits
the HTTPS → localhost traffic without extra flags.
