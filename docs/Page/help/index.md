---
layout: default
help: true
title: SquadSpeak help
description: Join channels, speak clearly and host conversations on your own computers.
---

# SquadSpeak help

SquadSpeak keeps voice, Markdown chat and portraits in one compact desktop app. A channel belongs to the computer hosting it. You do not need a chat account to join one.

## Join a conversation

Open SquadSpeak on both computers. Select **+**, choose a discovered channel and add it. If discovery cannot see the host, enter its DNS name or IP address. The host approves a new device and may ask for a channel password.

![A SquadSpeak channel with members and Markdown chat]({{ '/assets/help/channel-chat.png' | relative_url }})

Double-click a channel to join voice. Expand the row to read and write messages. A running app can host a channel while its owner participates elsewhere.

## Voice and audio

<a id="voice"></a>

Open **Settings > Input** to select a microphone and tune voice activation, echo cancellation, gain, noise reduction and frequency cuts. Each audio device keeps its own profile. Mute and push-to-talk still work while you adjust it.

![SquadSpeak input and audio settings]({{ '/assets/help/audio-settings.png' | relative_url }})

Use headphones when testing two copies on one computer. This avoids feeding one app's speakers into the other app's microphone.

## Chat and avatars

<a id="chat"></a>

Messages support Markdown, tables, links and images. Choose an avatar in your profile to make participants easy to recognise in a busy channel. Chat history belongs to the host and its retention policy.

![Avatar frames in SquadSpeak]({{ '/assets/help/avatar-frames.png' | relative_url }})

## Hosting

<a id="hosting"></a>

Your own channel remains available while the app is running. For a quiet server, start a headless host with a channel name and port. The host decides whether it accepts devices, requires a password and keeps message history.

For Internet hosting, forward both TCP and UDP on the selected port. Voice and screen sharing can fall back to TCP when UDP is unavailable.
