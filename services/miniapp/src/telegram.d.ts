// Minimal Telegram WebApp typings for the fields the player
// actually reads. This is not the full @types/telegram-web-app
// surface — only initDataUnsafe.chat, ready/expand, and the
// theme + haptic entry points we touch. Anything else the app
// needs later gets added here with the same restraint.
interface TelegramWebAppChat {
  id: number;
  type?: string;
  title?: string;
  username?: string;
}

interface TelegramWebAppInitData {
  chat?: TelegramWebAppChat;
  user?: { id: number; username?: string; first_name?: string };
  start_param?: string;
}

interface TelegramWebApp {
  initData: string;
  initDataUnsafe: TelegramWebAppInitData;
  ready: () => void;
  expand: () => void;
}

interface Window {
  Telegram?: {
    WebApp?: TelegramWebApp;
  };
}
