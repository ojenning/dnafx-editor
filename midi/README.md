Example of MIDI controller for dnafx-editor
===========================================

This folder contains a basic example showing how you can leverage the HTTP support in `dnafx-editor` to use MIDI as a way to control a DNAfx Git device, even if they don't natively support MIDI at all.

The example implements a super-simple Node.js application that uses [easymidi](https://www.npmjs.com/package/easymidi) to create a MIDI input, and then translates incoming MIDI messages to requests for the editor.

## Build

You'll need Node.js to run this demo. If you do, just run:

	npm install

to install the dependencies.

## Configuration

You configure which MIDI CC to intercept, and which values should do what, in `src/settings.js`. The default configuration file only reacts to CC `15`, and depending on the value of the controller message may send a request to the editor (e.g., when receiving a MIDI CC `15` with value `3`, it will send a `change-preset` for slot `59`). Check the configuration file to see how you can add your own actions.

You also need to configure where `dnafx-editor` is, that is its HTTP address. You do that in the same configuration file. The example provided in this repo assumes the editor is available locally (`127.0.0.1`) on port `8000`, which means the Node.js will send `POST` requests to `http://127.0.0.1:8000`.

## Running

Assuming you installed the dependencies and configured everything to your liking, you can start the demo by running:

	npm start

The controller will create a MIDI input named `DNAfx GiT MIDI Controller`. If everything worked, you should see this message on the terminal:

	Waiting for MIDI messages

A simle way to test if everything is working is using [sendmidi] to send a MIDI message to our controller. Assuming we're using the sample configuration file, using `sendmidi` like this:

	sendmidi dev "DNAfx GiT MIDI Controller" cc 15 3

should result in this output on the terminal:

	Got MIDI message: { channel: 0, controller: 15, value: 3, _type: 'cc' }
	  -- Associated action: { http: { request: 'change-preset', arguments: [ '59' ] } }

An HTTP message will be sent with that action to `dnafx-editor`, which means that if everything worked as expected you should see something like this on the editor:

	[HTTP] {"request":"change-preset","arguments":["59"]}
