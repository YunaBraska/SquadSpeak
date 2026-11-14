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
Play opens in one separate window, using the players' current voice-chat avatars
and names. Existing participants focus that window instead of joining again.

Setup proceeds by hovering and clicking one Trap, then one Core on distinct home
squares. Selection immediately readies the player and locks setup. Ready avatars
glow. When both are ready, one player's avatar indicates their turn. There is no
countdown or extra Ready button. First-fight and tied-fight element choices appear
beside the player's fighting piece. The information button provides concise rules
and tappable figure descriptions. Gameplay itself has no permanent rule paragraph.

The door button and window close request confirmation before leaving an active
game. Voice leave, channel switch and application quit use the same confirmation.
Cancel preserves both game and voice. Confirm leaves the game. In a two-player
game a known departure, disconnect or existing transport timeout awards the other
player a win, including during setup. A lone invitation is cancelled. Unknown host
shutdown ends the game without inventing a winner. There is no additional turn timer.
Victory and defeat show both avatars, remaining pieces and captured pieces.

Games reuse authenticated encrypted channel transport and its access rules. There
is no game server, database or account. The channel host remains authoritative,
while the participant who creates a game is its owner. Secret setup and pending
choices must stay private. Repeated or stale commands cannot create extra players,
turns, games or windows. Disconnect never automatically rejoins a game.

Only active games remain on the host. Results are sent once to participants and
removed from host memory. Clients retain current invitations and at most one
personal result, dismissed when its window closes or a new game starts. No game
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
ownership markers and shadows are rendered separately by the app. All figures
must remain distinct and uncropped in compact and large windows.

A compact water → fire → ice → water icon circle replaces the permanent rule
paragraph. The same icons become the nearby element choices during a fight.
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
Verify Leave, window reopening, disconnect, host stop, late readers and restart.
Reject observer actions, unrelated channels, private/external chats and invalid
commands. Check compact and large game windows, keyboard/touch controls, reduced
motion, actual avatar consistency, adjacent element choices, close/leave/switch/quit
confirmation and cancellation, and voice/chat remaining usable during play.

## Later candidates

These use original SquadSpeak artwork and styling, not copied game assets:

- Light trails: steer a trail and avoid walls and other trails.
- Multiplayer Snake: collect food while avoiding growing snakes.
- Four in a row: a compact two-player turn-based game.
- Reversi: two players capture pieces by enclosing them.
- Dots and boxes: take turns completing little squares.
- Memory pairs: a shared board with alternating turns.
- Reaction duel: react to a signal, with early presses counting as a miss.
- Drawing guesses: one player draws while others guess in a game-specific input.

Small turn-based games are the simplest next addition. Real-time trails and Snake
need separate latency, input timing and capacity qualification. Reference examples
include [Board Game Arena's Connect Four](https://en.boardgamearena.com/gamepanel?game=connectfour)
and [AirConsole's group games](https://www.airconsole.com/games/games-for-groups).
