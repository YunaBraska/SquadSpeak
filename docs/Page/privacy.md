---
layout: default
title: Privacy
description: How SquadSpeak handles conversations and local settings.
---

<header class="page-heading">
  <p class="eyebrow">Privacy</p>
  <h1>How SquadSpeak uses your data</h1>
  <p>SquadSpeak has no central chat account, advertising, analytics, crash reporting or YunaBraska-hosted conversation service.</p>
</header>

<section class="section">
  <h2>Conversations and hosts</h2>
  <p>The host you choose receives your connection address, saved identity, display name, avatar and language, together with the traffic needed for the features you use. Public channel names can be discovered before joining. Approvals and bans apply to identities, not physical hardware.</p>
  <p>A native SquadSpeak host can access channel messages and the voice, radio or screen streams it distributes. Native private-message content is encrypted for the two participants. The relay stores encrypted content and routing metadata. Anyone you explicitly allow to remotely control your app can access the conversations available to that app.</p>
  <p>TeamSpeak connections use the selected server's protocol and policies. SquadSpeak's private-message encryption and blocking do not extend to TeamSpeak. Choose hosts and participants carefully. Recipients can retain content you send.</p>
</section>

<section class="section">
  <h2>Local storage and retention</h2>
  <p>SquadSpeak keeps identities, saved connections, permissions and preferences in your operating system's application-data and credential storage. A headless server can instead use an explicitly configured, protected identity file.</p>
  <p>Native chat history and attachments are stored on the host with encrypted content. Database indexes still contain timestamps and routing-related metadata. The host selects an expiry of 24 hours, 7 days or 30 days. Supporter hosts can also select 90, 180 or 360 days. Expired content is hidden immediately and removed in bounded background batches while the host runs. Previously assigned expiry times do not change when a setting changes.</p>
  <p>Automatically accumulated contacts and their preferences, saved connections, approvals and bans expire after a year without contact. Your own identity and device profiles remain until removed. TeamSpeak chat history is held for the current app session. Removing the app does not necessarily remove local settings or credential-store entries, and does not delete history held by other hosts or recipients.</p>
</section>

<section class="section">
  <h2>Audio, screen sharing and notifications</h2>
  <p>Microphone and screen capture require permission. Local audio processing does not upload recordings to an analysis service. Speaker audio used for echo cancellation stays local. Screen sharing sends the selected source and, only when enabled, its labelled app or computer audio to viewers of your host channel.</p>
  <p>Optional Apple translation and speech processing use local system models. Preparing a language or voice can require a download from Apple. Chat notifications pass the sender and message preview to your operating system and may appear on the lock screen, according to your notification settings.</p>
</section>

<section class="section">
  <h2>External services</h2>
  <p>Playing radio connects the hosting app to the selected station provider. The provider receives the normal connection metadata, including the host's IP address. Opening a link connects your browser to its destination.</p>
  <p>When you send an image URL, your app downloads that image from its provider before preparing the attachment. That provider receives your connection metadata. Reading a received message does not automatically download external images.</p>
  <p>The direct-download edition checks GitHub for updates. Optional GitHub sign-in checks the signed-in account's Supporter or contributor eligibility. The app stores its access credentials and eligibility record encrypted on your computer. Signing out clears that local record. You can also revoke the app's authorization in GitHub. GitHub receives these requests and applies its own privacy policy. The App Store edition has no external Supporter activation and uses Apple's update service.</p>
</section>

<section class="section">
  <h2>Your choices and contact</h2>
  <p>You can leave channels, stop sharing, remove saved connections, block a native private conversation and revoke remote-control permissions. Hosts can remove participants and manage bans. These controls have different scopes and do not erase content already received.</p>
  <p>For app or privacy questions, use <a href="{{ site.support }}">SquadSpeak support</a>. Public issues are not a private reporting channel. Do not post private messages, credentials, invite details or connection data there. For content on an independently operated host, contact its operator.</p>
  <p>Last updated: 29 October 2026.</p>
</section>
