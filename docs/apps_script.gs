// Smart Mirror Weight Tracker — Google Sheet sync endpoint (Google Apps Script).
//
// Setup:
//   1. Create a Google Sheet. In row 1 put headers:  Date | Weight (kg)
//   2. Extensions → Apps Script. Delete the sample, paste this whole file.
//   3. Make sure TOKEN below matches CLOUD_TOKEN in the firmware's config.h.
//   4. Deploy → New deployment → type "Web app":
//        - Execute as: Me
//        - Who has access: Anyone
//      Deploy, authorize, and copy the Web app URL (ends with /exec).
//   5. Paste that URL into CLOUD_URL in config.h.
//
// Test in a browser: opening the /exec URL should show "Smart Mirror sync is live".

const TOKEN = 'change-me';   // set to match CLOUD_TOKEN in esp32-mirror/src/secrets.h

function doPost(e) {
  try {
    var body = JSON.parse(e.postData.contents);
    if (body.token !== TOKEN) {
      return ContentService.createTextOutput('bad token');
    }
    var sheet = SpreadsheetApp.getActiveSpreadsheet().getActiveSheet();
    var when = body.ts ? new Date(body.ts * 1000) : new Date();
    sheet.appendRow([when, Number(body.kg)]);
    return ContentService.createTextOutput('ok');
  } catch (err) {
    return ContentService.createTextOutput('err: ' + err);
  }
}

// GET returns all readings as CSV "epoch,kg" (one per line) so the device can
// pull the sheet down (sheet = source of truth). Token via ?token=... query.
function doGet(e) {
  if (!e || !e.parameter || e.parameter.token !== TOKEN) {
    return ContentService.createTextOutput('bad token');
  }
  var sheet = SpreadsheetApp.getActiveSpreadsheet().getActiveSheet();
  var values = sheet.getDataRange().getValues();
  var out = [];
  for (var i = 1; i < values.length; i++) {   // skip header row
    var d = values[i][0], w = values[i][1];
    if (d === '' || w === '' || w === null) continue;
    var ts = Math.floor(new Date(d).getTime() / 1000);
    out.push(ts + ',' + Number(w));
  }
  return ContentService.createTextOutput(out.join('\n'))
      .setMimeType(ContentService.MimeType.TEXT);
}
