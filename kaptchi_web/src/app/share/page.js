import Link from 'next/link';

export default async function SharePage({ searchParams }) {
  // In Next.js 15, searchParams is a promise
  const params = await searchParams;
  const boardId = params?.id;

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
            <span style={{ width: '8px', height: '8px', borderRadius: '50%', backgroundColor: '#10b981', display: 'inline-block', boxShadow: '0 0 10px rgba(16,185,129,0.5)' }}></span>
            <span style={{ opacity: 0.8 }}>Connecting</span>
          </div>
        </div>
      </header>

      {/* Canvas Area */}
      <main style={{ flex: 1, position: 'relative', overflow: 'hidden', display: 'flex', alignItems: 'center', justifyContent: 'center', backgroundColor: '#000' }}>
        
        {/* Placeholder Canvas Content */}
        {!boardId ? (
          <div className="animate-fade-in" style={{ textAlign: 'center', color: 'white' }}>
            <div style={{ width: '64px', height: '64px', borderRadius: '50%', background: 'rgba(255,255,255,0.1)', display: 'flex', alignItems: 'center', justifyContent: 'center', margin: '0 auto 1rem' }}>
              <svg width="32" height="32" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round"><circle cx="12" cy="12" r="10"></circle><line x1="12" y1="8" x2="12" y2="12"></line><line x1="12" y1="16" x2="12.01" y2="16"></line></svg>
            </div>
            <h2 style={{ fontFamily: 'var(--font-outfit)', fontSize: '1.5rem', marginBottom: '0.5rem' }}>No Board ID Provided</h2>
            <p style={{ opacity: 0.7 }}>Please provide a valid share link to view the canvas.</p>
            <Link href="/" className="btn btn-primary" style={{ marginTop: '1.5rem' }}>Go Home</Link>
          </div>
        ) : (
          <div className="animate-fade-in" style={{ textAlign: 'center', color: 'white', display: 'flex', flexDirection: 'column', alignItems: 'center' }}>
            {/* Loading Spinner */}
            <div style={{ width: '40px', height: '40px', border: '3px solid rgba(255,255,255,0.1)', borderTopColor: 'var(--primary)', borderRadius: '50%', margin: '0 auto 1.5rem', animation: 'spin 1s linear infinite' }}></div>
            <h2 style={{ fontFamily: 'var(--font-outfit)', fontSize: '1.5rem', marginBottom: '0.5rem' }}>Waiting for Whiteboard Data</h2>
            <p style={{ opacity: 0.7, maxWidth: '400px' }}>
              Waiting to stream whiteboard data for session <strong>{boardId}</strong>. 
              The backend implementation will render the canvas here.
            </p>
          </div>
        )}

        <style dangerouslySetInnerHTML={{__html: `
          @keyframes spin {
            to { transform: rotate(360deg); }
          }
        `}} />
      </main>
    </div>
  );
}
