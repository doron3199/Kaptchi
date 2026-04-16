"use client";

import { useEffect, useRef, useState } from 'react';
import Link from 'next/link';

export default function LiveCanvas({ boardId }) {
  const canvasRef = useRef(null);
  const containerRef = useRef(null);
  const [status, setStatus] = useState('connecting'); // connecting, connected, disconnected, error

  // Interactivity State
  const [scale, setScale] = useState(1);
  const [position, setPosition] = useState({ x: 0, y: 0 });
  const [isDragging, setIsDragging] = useState(false);
  const [dragStart, setDragStart] = useState({ x: 0, y: 0 });
  const [invertColors, setInvertColors] = useState(false);

  // Handlers
  const handleWheel = (e) => {
    const scaleAmount = -e.deltaY * 0.002;
    let newScale = scale * (1 + scaleAmount);
    newScale = Math.max(0.1, Math.min(newScale, 15)); 
    setScale(newScale);
  };

  const handleMouseDown = (e) => {
    setIsDragging(true);
    setDragStart({ x: e.clientX - position.x, y: e.clientY - position.y });
  };
  
  const handleMouseMove = (e) => {
    if (isDragging) {
      setPosition({
        x: e.clientX - dragStart.x,
        y: e.clientY - dragStart.y
      });
    }
  };

  const handleMouseUp = () => {
    setIsDragging(false);
  };

  useEffect(() => {
    if (!boardId) {
      setStatus('error');
      return;
    }

    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const wsUrl = `${protocol}//${window.location.host}/api/ws?id=${boardId}`;
    
    let ws = new WebSocket(wsUrl);
    
    ws.onopen = () => {
      console.log('Connected to Live Canvas Stream');
      setStatus('connected');
    };

    ws.onmessage = async (event) => {
      if (event.data instanceof Blob) {
        const url = URL.createObjectURL(event.data);
        const img = new Image();
        img.onload = () => {
          const canvas = canvasRef.current;
          const container = containerRef.current;
          if (canvas && container) {
            const ctx = canvas.getContext('2d');
            
            // Handle aspect ratio & sizing preserving aspect
            const aspect = img.width / img.height;
            const containerAspect = container.clientWidth / container.clientHeight;
            
            let drawWidth = container.clientWidth;
            let drawHeight = container.clientHeight;
            
            if (containerAspect > aspect) {
               drawWidth = container.clientHeight * aspect;
            } else {
               drawHeight = container.clientWidth / aspect;
            }
            
            canvas.width = drawWidth;
            canvas.height = drawHeight;
            
            ctx.clearRect(0, 0, canvas.width, canvas.height);
            ctx.drawImage(img, 0, 0, canvas.width, canvas.height);
            URL.revokeObjectURL(url);
          }
        };
        img.src = url;
      }
    };

    ws.onclose = () => {
      console.log('Disconnected from Live Canvas Stream');
      setStatus('disconnected');
    };

    ws.onerror = (err) => {
      console.error('WebSocket Error', err);
      setStatus('error');
    };

    return () => {
      if (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING) {
        ws.close();
      }
    };
  }, [boardId]);

  return (
    <div style={{ height: '100vh', display: 'flex', flexDirection: 'column', backgroundColor: 'var(--background)' }}>
      {/* App Bar */}
      <header className="glass" style={{ padding: '1rem 1.5rem', display: 'flex', justifyContent: 'space-between', alignItems: 'center', borderBottom: '1px solid var(--border)' }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: '0.75rem' }}>
          <Link href="/" style={{ width: '32px', height: '32px', borderRadius: '8px', background: 'linear-gradient(135deg, var(--primary), var(--accent))', display: 'flex', alignItems: 'center', justifyContent: 'center', color: 'white', fontWeight: 'bold', fontSize: '1rem', textDecoration: 'none' }}>
            K
          </Link>
          <div style={{ display: 'flex', flexDirection: 'column' }}>
            <span style={{ fontFamily: 'var(--font-outfit)', fontSize: '1.1rem', fontWeight: '600', lineHeight: 1 }}>Kaptchi Share</span>
            <span style={{ fontSize: '0.75rem', color: 'var(--secondary-foreground)', opacity: 0.7 }}>Live Whiteboard</span>
          </div>
        </div>
        
        <div style={{ display: 'flex', alignItems: 'center', gap: '1rem' }}>
          {boardId && (
            <div style={{ padding: '0.25rem 0.75rem', background: 'var(--secondary)', borderRadius: '100px', fontSize: '0.8rem', fontFamily: 'monospace', opacity: 0.8 }}>
              ID: {boardId}
            </div>
          )}
          <div style={{ display: 'flex', alignItems: 'center', gap: '0.4rem', fontSize: '0.875rem' }}>
            {status === 'connecting' && (
              <>
                <span style={{ width: '8px', height: '8px', borderRadius: '50%', backgroundColor: '#f59e0b', display: 'inline-block', boxShadow: '0 0 10px rgba(245,158,11,0.5)', animation: 'pulse 1.5s infinite' }}></span>
                <span style={{ opacity: 0.8, color: '#f59e0b' }}>Connecting</span>
              </>
            )}
            {status === 'connected' && (
              <>
                <span style={{ width: '8px', height: '8px', borderRadius: '50%', backgroundColor: '#10b981', display: 'inline-block', boxShadow: '0 0 10px rgba(16,185,129,0.5)' }}></span>
                <span style={{ opacity: 0.8, color: '#10b981' }}>Live</span>
              </>
            )}
            {status === 'disconnected' && (
              <>
                <span style={{ width: '8px', height: '8px', borderRadius: '50%', backgroundColor: '#9ca3af', display: 'inline-block' }}></span>
                <span style={{ opacity: 0.8, color: '#9ca3af' }}>Offline</span>
              </>
            )}
            {status === 'error' && (
              <>
                <span style={{ width: '8px', height: '8px', borderRadius: '50%', backgroundColor: '#ef4444', display: 'inline-block', boxShadow: '0 0 10px rgba(239,68,68,0.5)' }}></span>
                <span style={{ opacity: 0.8, color: '#ef4444' }}>Error</span>
              </>
            )}
          </div>
          
          <button 
             onClick={() => setInvertColors(!invertColors)}
             style={{ 
               background: invertColors ? 'var(--foreground)' : 'transparent', 
               color: invertColors ? 'var(--background)' : 'var(--foreground)',
               border: '1px solid var(--border)', 
               padding: '0.4rem 0.8rem', 
               borderRadius: '8px', 
               cursor: 'pointer',
               fontSize: '0.8rem',
               fontWeight: '600',
               transition: 'all 0.2s',
               marginLeft: '1rem'
             }}
          >
            {invertColors ? 'Normal Colors' : 'Invert Colors'}
          </button>
        </div>
      </header>

      {/* Canvas Container */}
      <main 
        ref={containerRef} 
        onWheel={handleWheel}
        onMouseDown={handleMouseDown}
        onMouseMove={handleMouseMove}
        onMouseUp={handleMouseUp}
        onMouseLeave={handleMouseUp}
        style={{ 
          flex: 1, position: 'relative', overflow: 'hidden', display: 'flex', 
          alignItems: 'center', justifyContent: 'center', backgroundColor: '#000',
          cursor: isDragging ? 'grabbing' : 'grab'
        }}
      >
        <canvas 
          ref={canvasRef} 
          style={{ 
            boxShadow: '0 0 40px rgba(0,0,0,0.5)',
            transition: isDragging ? 'none' : 'transform 0.1s ease-out, opacity 0.3s ease',
            opacity: status === 'connected' ? 1 : 0.2,
            transform: `translate(${position.x}px, ${position.y}px) scale(${scale})`,
            filter: invertColors ? 'invert(1) hue-rotate(180deg)' : 'none'
          }} 
        />
        
        {status === 'connecting' && (
           <div style={{ position: 'absolute', display: 'flex', flexDirection: 'column', alignItems: 'center' }}>
             <div className="spinner" style={{ width: '40px', height: '40px', border: '3px solid rgba(255,255,255,0.1)', borderTopColor: 'var(--primary)', borderRadius: '50%', margin: '0 auto 1.5rem', animation: 'spin 1s linear infinite' }}></div>
             <p style={{ opacity: 0.7 }}>Awaiting Whiteboard Data...</p>
           </div>
        )}
        
        {status === 'disconnected' && (
           <div style={{ position: 'absolute', display: 'flex', flexDirection: 'column', alignItems: 'center' }}>
             <div style={{ width: '64px', height: '64px', borderRadius: '50%', background: 'rgba(255,255,255,0.1)', display: 'flex', alignItems: 'center', justifyContent: 'center', margin: '0 auto 1rem' }}>
                <svg width="32" height="32" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round"><path d="M10.73 5.08A10.43 10.43 0 0 1 12 5c7 0 10 7 10 7a13.16 13.16 0 0 1-1.67 2.68"></path><path d="M6.61 6.61A13.526 13.526 0 0 0 2 12s3 7 10 7a9.74 9.74 0 0 0 5.39-1.61"></path><line x1="2" y1="2" x2="22" y2="22"></line></svg>
             </div>
             <p style={{ opacity: 0.7 }}>Host Disconnected</p>
           </div>
        )}
      </main>

      <style dangerouslySetInnerHTML={{__html: `
        @keyframes spin {
          to { transform: rotate(360deg); }
        }
        @keyframes pulse {
          0%, 100% { opacity: 1; transform: scale(1); }
          50% { opacity: 0.5; transform: scale(1.2); }
        }
      `}} />
    </div>
  );
}
