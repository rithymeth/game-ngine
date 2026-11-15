# Dialogue

The dialogue kit runs conversations: a graph of lines, some of which ask the player to choose. It is an optional part of the
build (`AETHER_KIT_DIALOGUE`, on by default) and needs no other kit. What a line *causes* (a quest starts, an item changes hands) is
your game's business: the kit hands your script the line and a free word to react to.

## A conversation

A conversation is a `.adialogue` file under your content folder; it is cooked automatically.

```json
{
  "name": "Innkeeper",
  "start": "hello",
  "nodes": [
    { "id": "hello", "speaker": "Maren", "text": "Welcome, traveller.", "next": "ask" },
    { "id": "ask", "speaker": "Maren", "text": "A room for the night?",
      "choices": [ { "text": "Yes please", "next": "deal" }, { "text": "No", "next": "bye" }, { "text": "Leave" } ] },
    { "id": "deal", "text": "That is two coins.", "event": "take_coin", "next": "bye" },
    { "id": "bye", "text": "Safe roads." }
  ]
}
```

- `start` is the first node; leave it out and the first node listed is it.
- A node says `text` (required), optionally as a `speaker`. It leads to `next`, **or** offers `choices`, never both. With neither it is the last line.
- A choice with no `next` ends the conversation.
- `event` is a free word a script can read when the node is reached.
- Limits: 1000 nodes per conversation, 16 choices per node.

A conversation with a missing name, no nodes, a repeated node id, an unknown start or next node, an empty line or choice, or a node with both `next` and `choices` is a warning in the player that names the file.

## Running one

`DialogueSystem` keeps one conversation per listening entity (usually the player). `Start(listener, name)` begins it (replacing one that
runs); `Current(listener)` is the line now (speaker, text, event, choices); `Advance` moves on and `Choose(index)` answers (0 is the first
choice). `Advance` on a question is refused (`NeedsChoice`); advancing past the last line ends the conversation.

## From a script

A game normally advances a line when the player presses a key or clicks a choice; this loop only shows every call, walking a conversation to its end at once.

```lua
Dialogue.Start(player, "Innkeeper")
while Dialogue.IsActive(player) do
    print(Dialogue.Speaker(player) .. ": " .. Dialogue.Text(player))
    if Dialogue.Event(player) == "take_coin" then Inventory.Remove(player, "Coin", 2) end
    if Dialogue.ChoiceCount(player) > 0 then
        Dialogue.Choose(player, 1)          -- choices count from 1 in scripts
    else
        Dialogue.Advance(player)
    end
end
```

`Dialogue.Start`, `IsActive`, `Speaker`, `Text`, `Event`, `ChoiceCount`, `Choice`, `IsLast`, `Advance`, `Choose`, `End` and `Exists`
are in the Luau inventory.
