module.exports = {
	// MIDI settings
	midi: {
		// Example of intercepting a CC
		cc: {
			// Example of intercepting CC 15
			15: {
				0: {
					http: { request: 'change-preset', arguments: ['200'] },
				},
				1: {
					http: { request: 'change-preset', arguments: ['1'] },
				},
				2: {
					http: { request: 'change-preset', arguments: ['35'] },
				},
				3: {
					http: { request: 'change-preset', arguments: ['59'] },
				}
			}
		},
		// Example of intercepting a Program Change
		program: {
			// Program Change 0
			0: {
				http: { request: 'change-preset', arguments: ['200'] },
			},
			// Program Change 37
			37: {
				http: { request: 'change-preset', arguments: ['1'] },
			}
		}
	},
	// DNAfx editor backend
	http: {
		host: '127.0.0.1',
		port: 8000
	}
};
