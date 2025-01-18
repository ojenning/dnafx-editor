module.exports = {
	// MIDI settings
	midi: {
		cc15: {
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
	// DNAfx editor backend
	http: {
		host: '127.0.0.1',
		port: 8000
	}
};
