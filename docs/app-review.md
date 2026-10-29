# App Review preparation

Purpose: explain SquadSpeak's decentralized communication and existing safety
controls to App Review. This is a draft, not a submission or an approval claim.
The product decision is [U178 and O-31](specs/voice-chat.md).

## Reviewer explanation

SquadSpeak provides voice and text communication through hosts chosen by the
user. Users enter a host address or discover hosts on their local network.
There is no publisher-operated public message feed or random stranger matching.
A host may be private or publicly reachable, depending on its operator's setup.

Hosts are operated independently. The app publisher has no administrative
access to their conversations. A native SquadSpeak host can approve or deny
joining identities, require a password, remove participants, ban identities
and lift those bans. Headless hosts admit clients automatically after any
required password check, while still rejecting banned identities.
Restrictions apply to saved identities, not physical hardware. Under the
current retention policy, saved restrictions expire after 365 days without
contact. Creating a different identity does not inherit an existing approval.

Native private conversations are between two known peers on one host. Either
peer can block new messages in both directions. Each peer controls its own
block. If both have blocked, both must unblock before messaging resumes.
The management view retains access to blocks even without visible chat history.
Blocking does not erase older messages, which retain their expiry.

Private message content is encrypted for the two peers. The host relays and
stores encrypted envelopes and routing metadata, but cannot decrypt that
content in its relay role. Native channel chat has a different trust boundary:
the channel host can decrypt it. The publisher receives neither automatically.
TeamSpeak connections use the selected third-party server and its protocol.
SquadSpeak's private-message encryption and block controls do not apply to TS3.

The app does not centrally monitor conversations or apply blanket word
censorship. Input validation, image sanitation and size/rate limits protect
the application, but are not semantic filtering of objectionable content.
There is currently no in-app offensive-content reporting workflow.

We request clarification on the safeguards required under Guideline 1.2 for
this independently hosted communication client. Would a user-initiated report
to the relevant host operator, together with developer contact for app-related
issues and the existing access/block controls, address this architecture?
Which additional measures are required for private conversations and external
providers? Such reporting is proposed here, not presented as implemented.

## Before submission

- Resolve O-31 with App Review. Guideline 1.2 does not state a general private-chat
  or self-hosting exemption. Do not describe the current controls as sufficient
  without that review.
- If reporting is required, agree its recipient, handling and participant
  confirmation before implementing it. No new central moderation service is
  approved. Private content must not be exposed to a host without participant
  selection and confirmation.
- Native channel bans and native private blocks have different scopes. There
  is no general client-side text block for channel participants or TS3 peers.
  Setting someone's listening volume to zero is not a message block.
- Channel unban restores eligibility to connect. It does not notify a
  disconnected peer proactively. Do not claim full SS-179 completion.
- Finalize the deferred privacy policy and a working developer contact route.
  The repository's bug-report link is not an offensive-content report service.
- Supply a reachable review host with test identities and exact steps for the
  submitted build. Keep credentials and private addresses out of this file.
  Do not submit test-only controls or promise unavailable mobile behavior.

## Reference models

Checked 2026-10-29. These sources inform the proposed responsibility split,
not a guarantee that another application's review outcome applies here.

- [Apple Guideline 1.2](https://developer.apple.com/app-store/review/guidelines/#user-generated-content)
  requires filtering, reporting with timely responses, blocking and contact
  information. It does not prescribe central monitoring of every conversation.
- [TeamSpeak Community Guidelines](https://www.teamspeak.com/en/community-guidelines/)
  assign rules and reports to independent server operators. TeamSpeak generally
  does not monitor self-hosted servers, while retaining enforcement over its
  own accounts, services and licenses.
- [Mumble privacy policy](https://www.mumble.info/privacy/) distinguishes data
  sent to an independent server from data sent to the Mumble project. Its
  [official iOS client](https://www.mumble.info/downloads/) is unmaintained since
  2017 and is not evidence of current App Review acceptance.
- [Element FAQ](https://docs.element.io/latest/element-support/quick-start-guide/frequently-asked-questions/)
  distinguishes the client from independently operated homeservers.
  [Matrix reports](https://matrix.org/legal/code-of-conduct/) go to the reporting
  user's homeserver administrator.
