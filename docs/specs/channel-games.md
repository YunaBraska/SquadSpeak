# Channel games

Scope owner: SS-169. The first game is an ICQ RPS-inspired tactical board game,
reinterpreted with fire, water and ice. The owner confirmed these reconstructed
rules on 2026-11-14.

## Shared experience

Any participant in a native SquadSpeak voice channel can create and own a game.
Several games may run in one channel. Each identity participates in at most one
active game across channels and hosts. Starting or joining another game never
silently replaces the current game. Voice membership is required to receive
invitations or game state. External providers and private conversations do not
offer this action.

The chat invitation contains the creator, game name, element-circle icon and Join.
It disappears when full or cancelled. The same icon marks a channel with a game.
Play uses the channel's compact Chat / Game switch on every platform. Switching
views preserves the game and voice connection. There is no separate game window.
The Chat tab shows the channel's existing unread marker, including while playing.
It clears only through the normal visible-chat read receipt at the latest message.
The same voice avatars, speaking levels, animation preference and mute indicators
are used in chat and game. Existing participants open their game instead of joining again.

The creator can click the empty opponent seat and confirm a local computer opponent.
It occupies the second seat and removes the invitation. A concurrent human join and
computer request cannot create an extra player. The computer uses the same game
actions and only its redacted player view, never secret opposing roles or choices.
One host-owned timer paces computer actions beyond combat animation and cancels
work when the game ends. It creates no account, voice participant or persisted data.

Setup proceeds by hovering and clicking one Trap, then one Core on distinct home
squares. Selection immediately readies the player and locks setup. Ready avatars
glow. When both are ready, one player's avatar indicates their turn. There is no
countdown or extra Ready button. First-fight and tied-fight element choices appear
beside the player's fighting piece. The information button provides concise rules
and tappable figure descriptions. Gameplay itself has no permanent rule paragraph.

The door button requests confirmation before leaving an active game.
Switching to chat does not leave it. Voice leave, channel switch and application quit use the same confirmation.
Cancel preserves both game and voice. Confirm leaves the game. In a two-player
game a known departure, disconnect or existing transport timeout awards the other
player a win, including during setup. A lone invitation is cancelled. Unknown host
shutdown ends the game without inventing a winner. There is no additional turn timer.
Victory and defeat show both avatars, remaining pieces and captured pieces.

Games reuse authenticated encrypted channel transport and its access rules. There
is no game server, database or account. The channel host remains authoritative,
while the participant who creates a game is its owner. Secret setup and pending
choices must stay private. Clients serialize admission requests. The host rejects
simultaneous participation and stale turn actions, so neither can add a player or
advance a turn twice. Disconnect never automatically rejoins a game.

Only active games remain on the host. Results are sent once to participants and
removed from host memory. Clients retain current invitations and at most one
personal result, dismissed with its close action or when a new game starts. No game
cards enter persisted chat history. A broken connection may miss the remote result
and instead show a local disconnection outcome. No result-replay cache is added.
Late voice participants receive current open invitations. Capability negotiation
preserves baseline chat for clients without games under SS-165.

## Elemental board rules

These confirmed rules define SquadSpeak's game. They do not claim that every
historical ICQ detail has been recovered:

- Two players use a board seven columns wide and six rows high. Each starts with
  fourteen pieces in their two home rows.
- Each player selects one immobile target and one immobile trap. The
  neutral target name is **Core** (German **Kern**). The twelve movable pieces
  begin neutral. There is no random element assignment or element quota.
- At their first encounter with another movable piece, neutral pieces choose
  fire, water or ice privately. If only one piece is neutral, only its player
  chooses. An established element remains unchanged until a tie. Choices are
  revealed together once all required players have chosen. A locked choice
  cannot be changed or exposed early.
- Players alternate moving one movable piece one square horizontally or
  vertically. No diagonal move or jump is allowed.
- Water defeats fire, fire defeats ice, and ice defeats water. The losing piece
  is removed. Equal elements require both players to choose a new element
  privately, repeating if necessary until there is a winner. The surviving
  piece retains that element on subsequent moves and encounters.
- Opposing piece roles remain hidden until combat reveals them. The surviving
  piece's revealed element remains visible. Private setup and pending choices
  must never be included in the opponent's or an uninvolved reader's snapshot.
- A trap removes an attacker and remains on its square. Capturing the opposing
  Core or eliminating their movable pieces wins the match.

Fire, water and ice have distinct shapes as well as colors. Short melting,
extinguishing and freezing effects explain outcomes while keeping the board visible. Reduced motion retains a brief visible impact and fade without travel or shaking. Artwork and
effects follow SquadSpeak's style and do not reuse ICQ assets. Pieces use detailed,
dimensional character artwork matching the avatars, with transparent silhouettes.
Concealed opponents share one neutral character with no role clues. Selection,
ownership markers and shadows are rendered separately by the app. Each viewer
sees their own pieces on blue rounded bases and opponents on red angular bases.
Setup uses the hover preview without floating cell labels or an instruction
figure. All figures must remain distinct and uncropped in compact and large windows.

A compact water → fire → ice → water icon circle explains the rules in the
information panel and identifies invitations. The playing view has no permanent
rule paragraph or element circle. In compact landscape, the board uses the full
available height with Information and Leave in the side margins. The same
element icons become the nearby choices during a fight.
An accessible description preserves the complete rule. Movement, required choices,
combat outcomes and game end have short local cues through App/Bot Sounds.
Voice mute does not mute these cues. Disabled notification sounds or zero app
volume silence them. Opening an existing game or receiving unrelated chat traffic
must not replay old sounds. Movement visibly travels between squares. Combat has a short approach, impact and
losing-figure effect before revealing the result. The local board waits for that
brief transition before another action. Six original figure calls and combat cues
share App/Bot Sounds. There is no looping background music.

The host remains the trusted authority, as for channel chat. This feature does
not introduce a separate game service or claim protection against a modified
host inspecting its own authoritative game state.

## Research and scope

[Pocket Gamer's contemporary 2008 review](https://www.pocketgamer.com/rock-paper-scissors/review/)
describes the ICQ-linked mobile edition: fourteen pieces per side, twelve armed
pieces, player-selected trap and flag bearer, single-square turns, initially
hidden enemy weapons and a new choice when equal weapons meet. It is a firsthand
review of the mobile adaptation, not an original PC rules manual.

The current [RPS Ninja developer guide](https://rpsninja.uk/landing/de) documents
a classic 7×6 board, hidden setup, persistent traps and combat disclosure.
[Helpiqo's own rules](https://helpiqo.com/en/rps-duel) explicitly specify four
pieces per element, orthogonal movement and immobile flag/trap pieces. SquadSpeak's
later confirmed neutral-first-encounter rule replaces that initial distribution. These are
modern remakes and evidence for the proposed reconstruction, not authority for
every original ICQ detail.

The exact historical turn timeout, trap behavior and post-tie weapon persistence have not
been independently established from an original PC manual. Do not silently add
timeouts, advanced modes, rankings, spectators or additional games.

## Acceptance

Exercise participant-created invitations, voice-only disclosure, parallel games, full lobbies and
cross-channel/host attempts to join a second game. Verify private setup, invalid
or overlapping role selections, automatic readiness, legal/illegal moves, alternating
turns, neutral first encounters, one-sided choice against an established element,
all elemental outcomes, hidden tie choices, retained elements, trap and target encounters,
replayed commands and terminal outcomes through the actual UI and TLS path.
Verify Leave, view reopening, disconnect, host stop, late readers and restart.
Reject observer actions, unrelated channels, private/external chats and invalid
commands. Check compact and large channel views, keyboard/touch controls, reduced
motion, actual avatar consistency, adjacent element choices, close/leave/switch/quit
confirmation and cancellation, and voice/chat remaining usable during play.

An iPhone Simulator run in portrait and landscape is sufficient for the mobile
game-interface acceptance. A physical iPhone is not required for this journey.
Use native orientation and touch input, including Chat/Game switching, computer
confirmation, setup, moves, scrollable help and cancelled/confirmed departure.

## Later candidates

The next requested game is a multiplayer creature-and-trail game, followed by
a compact strategy game inspired by **Final Fantasy VII Rebirth**'s Fort Condor.
Both need their own name, original artwork and SquadSpeak styling.

### Creature-and-trail game

Confirmed on 2026-11-14: use score rounds with re-entry. Creatures collect fruit
and grow longer tails. A collision costs points and length, then the player
returns instead of waiting as a spectator for the rest of the round.

[Curve Fever's developer description](https://store.steampowered.com/app/1133700)
combines steering, trails, points and offensive/defensive powers. The official
[slither.io description](https://apps.apple.com/us/app/slither-io/id1091944550)
uses collecting, growth and head-to-body collisions. These are references for
risk and interaction, not rules to copy wholesale.

Proposed direction: warm avatar creatures, fruit and scarf- or vine-like tails,
with a small number of readable interactions. The working name **Tangletrail**
is a proposal. The round end condition, duration, capacity, collision penalties,
own-tail and edge behavior, controls and departure rules remain undecided.
Validate these with a playable keyboard/touch prototype before production integration.

### Compact strategy game

[Square Enix's Rebirth description](https://www.square-enix.com/ffvii/en-gb/games/rebirth/world/)
describes deploying units, defending a base, attacking the opposing base and
commander abilities. Rebirth is the requested reference, not Intergrade.

Proposed presentation: a small 2.5D diorama with original dimensional character
sprites, short walk cycles, shadows, smooth movement and clear impact effects.
This can use the existing Qt Quick rendering approach without a new 3D engine.
Unit roles, routes, resource rules, capacity and victory conditions need a
separate agreed ruleset. No Fort Condor assets or names become game content.

### Shared implementation boundary

Reuse channel authorization, invitations, one active game per identity,
Chat/Game navigation, voice portraits and confirmed departure where behavior
matches. Current board rules and views are Elements-specific. Real-time trails
need a host-owned simulation clock, validated inputs, bounded network updates
and latency/capacity tests. Strategy needs its own unit and combat rules.
Do not build a generic engine or introduce a runtime dependency before a
complete second game demonstrates which behavior is actually shared.

### Other ideas

These use original SquadSpeak artwork and styling, not copied game assets:

- Light trails: steer a trail and avoid walls and other trails.
- Four in a row: a compact two-player turn-based game.
- Reversi: two players capture pieces by enclosing them.
- Dots and boxes: take turns completing little squares.
- Memory pairs: a shared board with alternating turns.
- Reaction duel: react to a signal, with early presses counting as a miss.
- Drawing guesses: one player draws while others guess in a game-specific input.

Smaller turn-based alternatives remain ideas rather than the next priority.
Reference examples include
[Board Game Arena's Connect Four](https://en.boardgamearena.com/gamepanel?game=connectfour)
and [AirConsole's group games](https://www.airconsole.com/games/games-for-groups).
