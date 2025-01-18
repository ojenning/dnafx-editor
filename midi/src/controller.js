const easymidi = require('easymidi');
const http = require('http');

const midi = new easymidi.Input('DNAfx GiT MIDI Controller', true);

const settings = require('./settings.js');

function sendHttpPost(data) {
	// Prepare the HTTP message to send to the editor
	let dataJson = JSON.stringify(data);
	let options = {
		method: 'POST',
		hostname: settings.http.host,
		port: settings.http.port,
		path: '/',
		headers: {
			'Content-Type': 'application/json',
			'Content-Length': dataJson.length
		}
	};
	let httpResponse = function(response) {
		// Continuously update stream with data
		let body = '';
		response.on('data', function(d) {
			body += d;
		});
		response.on('end', function() {
			// Data reception is done, send the response back to the callback
			try {
				let json = JSON.parse(body);
				console.dir(json);
			} catch(err) {
				console.error(err);
			}
		});
	};
	let request = http.request(options, httpResponse);
	request.on('error', function(err) {
		console.error(err);
	});
	request.write(dataJson);
	request.end();
}

async function init() {
	// Initialize MIDI events and how to react
	midi.on('cc', function(cc) {
		console.log('Got MIDI message:', cc);
		let actions = settings.midi['cc' + cc.controller];
		if(!actions)
			return;
		let action = actions[cc.value];
		if(!action)
			return;
		console.log('  -- Associated action:', action);
		if(action.http)
			sendHttpPost(action.http);
	});
	console.log('Waiting for MIDI messages');
}

init();
