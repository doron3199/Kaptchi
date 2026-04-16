import { Inter, Outfit } from "next/font/google";
import "./globals.css";

const inter = Inter({
  variable: "--font-inter",
  subsets: ["latin"],
});

const outfit = Outfit({
  variable: "--font-outfit",
  subsets: ["latin"],
});

export const metadata = {
  title: "Kaptchi - Whiteboard Assistant Software",
  description: "Whiteboard assistant software designed specifically for users with low vision who have difficulty with distance vision. Easily capture and read whiteboards.",
  openGraph: {
    title: "Kaptchi - Whiteboard Assistant",
    description: "Notice: If you rely on a screen reader, this software is unfortunately not for you. It is designed specifically for users with low vision who have difficulty with distance vision.",
  }
};

export default function RootLayout({ children }) {
  return (
    <html
      lang="en"
      className={`${inter.variable} ${outfit.variable}`}
      suppressHydrationWarning
    >
      <body>{children}</body>
    </html>
  );
}
