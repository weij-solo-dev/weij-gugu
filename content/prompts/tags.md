# Expression tags (closed vocabulary, 13)

The model writes one tag in square brackets at the start of a sentence. The firmware strips it,
switches the face / plays the action, and voices the rest. Unknown tags are dropped silently.

emotions: neutral happy laughing surprised curious shy sad sleepy
actions:  coo puff whistle wave nod

Rules: ASCII names only; no tag means "keep the previous one"; no emoji; never read aloud.
