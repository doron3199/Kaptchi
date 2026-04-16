import { createServer } from 'http';
import { parse } from 'url';
import next from 'next';
import { WebSocketServer } from 'ws';

const dev = process.env.NODE_ENV !== 'production';
const hostname = dev ? 'localhost' : '0.0.0.0';
const port = process.env.PORT || 3000;
// when using middleware `hostname` and `port` must be provided below
const app = next({ dev, hostname, port });
const handle = app.getRequestHandler();

app.prepare().then(() => {
  const server = createServer(async (req, res) => {
    try {
      const parsedUrl = parse(req.url, true);
      await handle(req, res, parsedUrl);
    } catch (err) {
      console.error('Error occurred handling', req.url, err);
      res.statusCode = 500;
      res.end('internal server error');
    }
  });

  // Attach WebSocket Server
  const wss = new WebSocketServer({ noServer: true });

  // Map of boardId -> Set of WebSockets
  const boardViewers = new Map();
  const boardHosts = new Set();

  wss.on('connection', (ws, req, boardId, role) => {
    console.log(`[WS] Client connected for board: ${boardId}, role: ${role || 'viewer'}`);
    
    if (role === 'host') {
      if (boardHosts.has(boardId)) {
        console.log(`[WS] Rejecting secondary host for board: ${boardId}`);
        ws.close(1008, 'Room already has a host');
        return;
      }
      boardHosts.add(boardId);
    }
    
    if (!boardViewers.has(boardId)) {
      boardViewers.set(boardId, new Set());
    }
    boardViewers.get(boardId).add(ws);

    // Expecting binary data (JPEG frames) from the source
    if (role === 'host') {
      ws.on('message', (message, isBinary) => {
        // Broadcast this message to all viewers for this boardId
        const viewers = boardViewers.get(boardId);
        if (viewers) {
          viewers.forEach((client) => {
            if (client !== ws && client.readyState === 1) { // 1 = OPEN
              client.send(message, { binary: isBinary });
            }
          });
        }
      });
    }

    ws.on('close', () => {
      console.log(`[WS] Client disconnected from board: ${boardId}, role: ${role || 'viewer'}`);
      if (role === 'host') {
        boardHosts.delete(boardId);
      }
      if (boardViewers.has(boardId)) {
        boardViewers.get(boardId).delete(ws);
        if (boardViewers.get(boardId).size === 0 && !boardHosts.has(boardId)) {
          boardViewers.delete(boardId);
        }
      }
    });
  });

  server.on('upgrade', (req, socket, head) => {
    const { pathname, query } = parse(req.url || '/', true);

    if (pathname === '/api/ws' && query.id) {
      wss.handleUpgrade(req, socket, head, (ws) => {
        wss.emit('connection', ws, req, query.id, query.role);
      });
    } else if (!pathname.startsWith('/_next')) {
      socket.destroy();
    }
  });

  server.listen(port, () => {
    console.log(`> Ready on http://${hostname}:${port}`);
    console.log(`> WebSocket Server ready on ws://${hostname}:${port}/api/ws`);
  });
});
