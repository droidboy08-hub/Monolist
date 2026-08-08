/** @type {import('tailwindcss').Config} */
export default {
  content: [
    "./index.html",
    "./src/**/*.{js,ts,jsx,tsx}",
  ],
  theme: {
    extend: {
      colors: {
        background: '#050505',
        surface: '#111111',
        primary: '#1DB954',
        accent: '#E9DDFF',
        secondary: '#8E8E93',
      },
      borderRadius: {
        'mobile': '32px',
      }
    },
  },
  plugins: [],
}
