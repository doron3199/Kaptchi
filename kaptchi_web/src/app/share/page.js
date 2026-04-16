import LiveCanvas from './LiveCanvas';

export default async function SharePage({ searchParams }) {
  // In Next.js 15, searchParams is a promise
  const params = await searchParams;
  const boardId = params?.id;

  if (!boardId) {
    return (
      <div style={{ height: '100vh', display: 'flex', flexDirection: 'column', backgroundColor: 'var(--background)', alignItems: 'center', justifyContent: 'center' }}>
        <div className="animate-fade-in" style={{ textAlign: 'center', color: 'white' }}>
          <div style={{ width: '64px', height: '64px', borderRadius: '50%', background: 'rgba(255,255,255,0.1)', display: 'flex', alignItems: 'center', justifyContent: 'center', margin: '0 auto 1rem' }}>
            <svg width="32" height="32" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round"><circle cx="12" cy="12" r="10"></circle><line x1="12" y1="8" x2="12" y2="12"></line><line x1="12" y1="16" x2="12.01" y2="16"></line></svg>
          </div>
          <h2 style={{ fontFamily: 'var(--font-outfit)', fontSize: '1.5rem', marginBottom: '0.5rem' }}>No Board ID Provided</h2>
          <p style={{ opacity: 0.7 }}>Please provide a valid share link to view the canvas.</p>
          <a href="/" className="btn btn-primary" style={{ marginTop: '1.5rem', display: 'inline-block', padding: '0.75rem 1.5rem', background: 'var(--primary)', color: 'black', borderRadius: '8px', textDecoration: 'none', fontWeight: 'bold' }}>Go Home</a>
        </div>
      </div>
    );
  }

  return <LiveCanvas boardId={boardId} />;
}
