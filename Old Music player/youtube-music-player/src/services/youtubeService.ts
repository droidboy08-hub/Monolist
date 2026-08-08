// Mock service to test if imports are the cause of the crash
// import yts from 'yt-search';
// import ytdl from 'ytdl-core';

export interface Song {
  id: string;
  title: string;
  artist: string;
  thumbnail: string;
  duration: string;
  url: string;
}

export const searchYouTube = async (query: string): Promise<Song[]> => {
  console.log('Mock search for:', query);
  return [
    {
      id: '1',
      title: 'Mock Song 1',
      artist: 'Mock Artist',
      thumbnail: 'https://via.placeholder.com/150',
      duration: '3:00',
      url: '#'
    }
  ];
};

export const getStreamUrl = async (videoId: string): Promise<string | null> => {
  console.log('Mock stream for:', videoId);
  return null;
};
