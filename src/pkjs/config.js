module.exports = [
  {
    type: 'heading',
    defaultValue: 'PiFire Remote'
  },
  {
    type: 'text',
    defaultValue: 'Your phone talks to PiFire (v1.7 or newer) directly, so it needs to be on the same network as the grill, or able to reach it over a VPN.'
  },
  {
    type: 'section',
    items: [
      {
        type: 'heading',
        defaultValue: 'Connection'
      },
      {
        type: 'input',
        messageKey: 'PIFIRE_URL',
        label: 'PiFire address',
        defaultValue: '',
        attributes: {
          placeholder: 'http://pifire.local',
          type: 'url'
        }
      }
    ]
  },
  {
    type: 'submit',
    defaultValue: 'Save'
  }
];
