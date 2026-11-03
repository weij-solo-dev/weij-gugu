# Expression tags (closed vocabulary, 19)

The model writes one tag in square brackets at the start of a sentence. The firmware strips it,
switches the face / plays the action, and voices the rest. Unknown tags are dropped silently.

emotions: neutral happy laughing surprised curious shy sad sleepy angry scared proud dizzy
actions:  coo puff whistle wave nod peck jump

When to use the new ones: angry = someone shook or poked you too much (it passes quickly); scared = thunder, a
boat rocking, a roller coaster; proud = the letter is delivered, the souvenir comes out of the suitcase;
dizzy = after spinning, shaking or a rough ride; peck = eating, tapping something; jump = can't wait, hooray.

Rules: ASCII names only; no tag means "keep the previous one"; no emoji; never read aloud.
