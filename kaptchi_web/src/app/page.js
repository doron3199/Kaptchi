import Image from "next/image";

export default function Home() {
  return (
    <>
      {/* Header / Nav */}
      <header className="container" style={{ padding: '2rem 1.5rem', display: 'flex', alignItems: 'center' }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: '0.75rem' }}>
          <Image src="/logo.png" alt="Kaptchi Logo" width={40} height={40} style={{ borderRadius: '10px' }} />
          <span style={{ fontFamily: 'var(--font-outfit)', fontSize: '1.5rem', fontWeight: '700' }}>Kaptchi</span>
        </div>
      </header>

      <main style={{ flex: 1 }}>
        {/* Hero Section */}
        <section className="section container text-center animate-fade-in" style={{ paddingBottom: '3rem' }}>
          <div style={{ maxWidth: '800px', margin: '0 auto' }}>
            <div style={{ display: 'inline-block', padding: '0.5rem 1rem', background: 'rgba(59, 130, 246, 0.1)', color: 'var(--primary)', borderRadius: '100px', fontSize: '0.875rem', fontWeight: '600', marginBottom: '1.5rem' }}>
              Open Source Accessibility Tool
            </div>
            <h1 style={{ fontSize: 'clamp(2.5rem, 5vw, 4rem)', marginBottom: '1.5rem' }}>
              Whiteboard assistant software<br />
              <span className="text-gradient">for the visually impaired.</span>
            </h1>
            
            <div className="flex justify-center gap-4" style={{ flexWrap: 'wrap', marginBottom: '3rem' }}>
              <a href="https://github.com/doron3199/Kaptchi/releases/download/v1.1.1/KaptchiSetup.exe" className="btn btn-primary" style={{ fontSize: '1.1rem' }}>
                <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round" style={{ marginRight: '0.5rem' }}>
                  <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"></path>
                  <polyline points="7 10 12 15 17 10"></polyline>
                  <line x1="12" y1="15" x2="12" y2="3"></line>
                </svg>
                Install for Windows
              </a>
              <a href="https://github.com/doron3199/Kaptchi" target="_blank" rel="noopener noreferrer" className="btn btn-secondary" style={{ fontSize: '1.1rem' }}>
                View Source
              </a>
            </div>

            <div style={{ borderRadius: 'var(--radius)', overflow: 'hidden', boxShadow: '0 20px 40px rgba(0,0,0,0.4)', marginBottom: '2rem' }}>
              <Image src="/images/demostration.png" alt="Demonstration of Kaptchi in a classroom" width={1200} height={600} style={{ width: '100%', height: 'auto', objectFit: 'cover' }} />
            </div>

            <div style={{ margin: '0 auto', maxWidth: '800px', padding: '1rem', border: '1px solid var(--border)', borderRadius: 'var(--radius)', background: 'var(--secondary)', textAlign: 'left', display: 'flex', gap: '1rem', alignItems: 'flex-start' }}>
              <div style={{ color: '#eab308', marginTop: '0.25rem' }}>
                <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
                  <path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"></path>
                  <line x1="12" y1="9" x2="12" y2="13"></line>
                  <line x1="12" y1="17" x2="12.01" y2="17"></line>
                </svg>
              </div>
              <div>
                <strong style={{ display: 'block', marginBottom: '0.25rem' }}>Notice</strong>
                <p style={{ margin: 0, fontSize: '0.95rem', opacity: 0.8 }}>
                  If you rely on a screen reader, this software is unfortunately not for you. It is designed specifically for users with low vision who have difficulty with distance vision.
                </p>
              </div>
            </div>
          </div>
        </section>

        {/* Feature Blocks (Replacing the cards with alternating image/text blocks to match original site) */}
        <section className="container animate-fade-in" style={{ padding: '2rem 1.5rem', maxWidth: '1000px', margin: '0 auto', display: 'flex', flexDirection: 'column', gap: '6rem' }}>
          
          <div style={{ display: 'flex', flexDirection: 'column', gap: '2rem', alignItems: 'center' }}>
            <div style={{ width: '100%', borderRadius: 'var(--radius)', overflow: 'hidden', border: '1px solid var(--border)' }}>
              <Image src="/images/homepage.png" alt="Kaptchi Available Cameras" width={1000} height={600} style={{ width: '100%', height: 'auto' }} />
            </div>
            <div style={{ textAlign: 'center', maxWidth: '800px' }}>
              <h3 style={{ fontSize: '1.75rem', marginBottom: '1rem', fontFamily: 'var(--font-outfit)' }}>Versatile Capture Methods</h3>
              <p style={{ fontSize: '1.1rem', color: 'var(--secondary-foreground)', opacity: 0.9, lineHeight: 1.6 }}>
                You can choose between an integrated camera, a mobile camera using the Kaptchi Android app, a whole monitor, or a single window. Create a virtual monitor if you have only one screen, but still need to capture a different window.
              </p>
            </div>
          </div>

          <div style={{ display: 'flex', flexDirection: 'column', gap: '2rem', alignItems: 'center' }}>
            <div style={{ width: '100%', display: 'flex', flexDirection: 'column', gap: '1rem' }}>
              <div style={{ borderRadius: 'var(--radius)', overflow: 'hidden', border: '1px solid var(--border)' }}>
                <Image src="/images/filters.png" alt="Kaptchi Filters" width={1000} height={400} style={{ width: '100%', height: 'auto' }} />
              </div>
              <div style={{ borderRadius: 'var(--radius)', overflow: 'hidden', border: '1px solid var(--border)' }}>
                <Image src="/images/raw_image.png" alt="Raw Whiteboard Image" width={1000} height={400} style={{ width: '100%', height: 'auto' }} />
              </div>
            </div>
            <div style={{ textAlign: 'center', maxWidth: '800px' }}>
              <h3 style={{ fontSize: '1.75rem', marginBottom: '1rem', fontFamily: 'var(--font-outfit)' }}>Smart Processing</h3>
              <p style={{ fontSize: '1.1rem', color: 'var(--secondary-foreground)', opacity: 0.9, lineHeight: 1.6 }}>
                Use powerful filters and person removal software to get a clean whiteboard image without obstructions. (Thanks <a href="https://www.youtube.com/watch?v=fYyARMqiaag" target="_blank" rel="noopener noreferrer" style={{ textDecoration: 'underline', color: 'var(--primary)' }}>Professor Leonard!</a>)
              </p>
            </div>
          </div>

          <div style={{ display: 'flex', flexDirection: 'column', gap: '2rem', alignItems: 'center' }}>
            <div style={{ width: '100%', display: 'flex', flexDirection: 'column', gap: '1rem' }}>
              <div style={{ borderRadius: 'var(--radius)', overflow: 'hidden', border: '1px solid var(--border)' }}>
                <Image src="/images/drawing.png" alt="Kaptchi Drawing Features" width={1000} height={500} style={{ width: '100%', height: 'auto' }} />
              </div>
              <div style={{ borderRadius: 'var(--radius)', overflow: 'hidden', border: '1px solid var(--border)' }}>
                <Image src="/images/share_bar.png" alt="Kaptchi Share Bar" width={1000} height={300} style={{ width: '100%', height: 'auto' }} />
              </div>
            </div>
            <div style={{ textAlign: 'center', maxWidth: '800px' }}>
              <h3 style={{ fontSize: '1.75rem', marginBottom: '1rem', fontFamily: 'var(--font-outfit)' }}>Edit & Export</h3>
              <p style={{ fontSize: '1.1rem', color: 'var(--secondary-foreground)', opacity: 0.9, lineHeight: 1.6 }}>
                Cut and draw on "whiteboard screenshots", then export all of them in one PDF file.
              </p>
            </div>
          </div>

        </section>
      </main>

      {/* Footer */}
      <footer style={{ borderTop: '1px solid var(--border)', padding: '3rem 0', marginTop: '4rem' }}>
        <div className="container flex flex-col items-center justify-between" style={{ gap: '1.5rem', flexWrap: 'wrap', flexDirection: 'row' }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: '0.75rem', opacity: 0.7 }}>
            <Image src="/logo.png" alt="Kaptchi Logo" width={24} height={24} style={{ borderRadius: '6px' }} />
            <span style={{ fontWeight: '600' }}>Kaptchi</span>
          </div>
          
          <div style={{ opacity: 0.7, fontSize: '0.9rem', textAlign: 'center' }}>
            Open Source. Contributions are welcome!
          </div>

          <div style={{ display: 'flex', gap: '1.5rem' }}>
            <a href="https://github.com/doron3199/Kaptchi" target="_blank" rel="noopener noreferrer" style={{ opacity: 0.7, textDecoration: 'none' }} className="hover:text-primary transition-colors">GitHub</a>
            <a href="mailto:classboards2@gmail.com" style={{ opacity: 0.7, textDecoration: 'none' }} className="hover:text-primary transition-colors">classboards2@gmail.com</a>
          </div>
        </div>
      </footer>
    </>
  );
}
