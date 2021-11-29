# QtDiscordIPC
Discord RPC/IPC API for Qt implemented using QLocalSocket.
This API allows to control the locally running Discord application. It's not particularly well implemented, I've made it for my [Discord Volume Mixer for Stream Deck project](https://github.com/CZDanol/streamdeck-discordmixer).

API documentation: https://discord.com/developers/docs/topics/rpc
Uses OAuth authentication. After authentized, stores the auth data in discordOauth.json so that the app doesn't have to authentize each time (TODO: auth renewal, it kinda screws up currently).

## Requirements
Tested on MSVC 2019 x64, Qt 6.2.1, C++17.
Requires Qt Core and Network.

## Usage
The API is blocking, so everything's quite simple. Just beware that it might run QEventLoop locally for the blocking operations.

1. Create an app on the Discord Developer Portal.
2. Set the redirect URI to `http://localhost:1337/callback`
3. Copy Client ID and Client Secret from the Oauth2 tab on the portal and give it to the application.