# Google Sign In

This plugin is used with the pub.dev package `google_sign_in`
https://pub.dev/packages/google_sign_in

# Configuration

Google Cloud Console https://console.cloud.google.com

1. Create new Cloud project

* Enable People API - minimum API REQUIRED
* Create OAuth 2.0 Client Credentials
    * Download OAuth client
* Set environmental variables:
    * GOOGLE_API_OAUTH2_CLIENT_SECRET_JSON
        * this should be set to the absolute file path of the json file downloaded from the cloud console. User must
          have read access.
    * GOOGLE_API_OAUTH2_CLIENT_CREDENTIALS
        * absolute filepath of where to cache tokens. User must have read/write access.

2. Run project once to get the authorization URL. Click through till you get the authorization code. Copy this and add
   to value of key auth_code in GOOGLE_API_OAUTH2_CLIENT_CREDENTIALS.
3. Re-Run the project

## Functional Test Case

https://github.com/flutter/packages/tree/main/packages/google_sign_in/google_sign_in/example
