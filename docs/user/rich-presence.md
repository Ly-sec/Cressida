---
title: Rich Presence
description:
  Share activity from local games and music players, or create a custom
  activity.
---

Open **Settings > Activity Settings > Activity Privacy** to manage local
application activity. **Enable RPC** starts on for each account. Compatible
applications can publish activities while Cressida is signed in. Availability
and custom status remain separate from these activities.

## Local applications

Start a compatible game or music player and enable its activity integration.
Cressida accepts local IPC activity connections from applications running as
your desktop user. The settings page lists applications that have submitted an
activity, with **Block** and **Allow** controls for individual applications.

Turning **Enable RPC** off clears local publications and closes the local
connection. **Activity Privacy > Share current activity** controls account
visibility as well. Cressida does not scan running processes to detect games.

Application names and uploaded artwork keys are resolved using the application's
public ID. HTTPS artwork is resolved before publication. A failed lookup has an
explicit **Try again** action. A local preview shows the activity Cressida has
prepared; it does not confirm what another client has received.

## Activity displays

Activities appear beneath names in direct-message and member lists, and in the
account area at the bottom left. Profiles and the right-side DM profile panel
show activity cards with available details, artwork and link buttons. When an
activity has no custom artwork, its registered application icon is used when
available. Image display preferences apply to both.

Some activities provide button labels separately from their links. These buttons
are still shown. Clicking one fetches its current link and opens it in your
default browser. Failed reads require another click to retry. A delayed response
cannot open a link after the activity changes or your account is replaced.

When an application supplies both a start and an end time, its card shows
elapsed time, total duration and a progress bar. A start time alone shows
elapsed time; an end time alone shows remaining time. The display updates while
the card is visible. It reflects application timestamps, so pausing or seeking
in a player requires the player to send an updated activity.

## Custom activity

Cressida shows your own activity buttons as a local preview. Other clients may
hide buttons on your own profile. Check from another person's account when
verifying that the published buttons are visible to others.

Expand **Custom activity** to open the editor. It starts collapsed. Enter your
application's public ID and choose Playing, Listening, Watching or Competing.
Optional fields include details, state, artwork, timestamps, party information
and up to two HTTPS link buttons.

The application ID must identify an existing application. Use **Developer
portal** in the editor to create or open one and copy its **Application ID**.
Your user ID, account token and client secret are not application IDs. No token
or secret is needed in this field. A missing application returns **Application
not found** and cannot be published; editing the Name field does not register an
application.

**Save** stores the editor values. **Start** saves and publishes them. **Stop**
ends the custom activity and lets local applications of the same type appear
again. A custom activity takes priority over local applications of its type.
Saved custom activities never start automatically after signing in again.

Starting a custom activity with both time fields empty starts an elapsed timer
at that moment. Stopping and starting it again resets that timer. The generated
start time is not saved in the editor. Explicit time fields take precedence.

Activity times use Unix seconds. Artwork can use uploaded application asset keys
or HTTPS image URLs. Session secrets stay in memory and are excluded from saved
custom activities. Entering a secret does not enable game joining or spectating.

## Scope and troubleshooting

The local connection supports activity publication, clearing and activity-event
subscriptions. General account remote control, game invitations, joining and
spectating are not implemented. These features are separate from activity link
buttons, which open in your default browser.

Another running client may own the first local IPC socket. Quit that client and
restart your activity application if it attaches to the wrong one. Applications
inside containers or sandboxes must be able to reach your desktop's runtime
socket. A failed connection is shown on the Activity Privacy settings page.

Local settings are saved per account. Active connections and activities are
cleared on sign-out. See [Files and local data](files-and-data.md).
