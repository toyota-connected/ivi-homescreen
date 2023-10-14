# Google Sign In

This static plugin is to support this pub.dev package:
https://pub.dev/packages/google_sign_in

# Configuration
Google Cloud Console https://console.cloud.google.com
1. Create new Cloud project
  * Enable People API - minimum API REQUIRED
  * Create OAuth 2.0 Client Credentials
    * Download OAuth client
  * Set environmental variables:
    * GOOGLE_API_OAUTH2_CLIENT_SECRET_JSON
      * this should be set to the absolute file path of the json file downloaded from the cloud console.  User must have read access.
    * GOOGLE_API_OAUTH2_CLIENT_CREDENTIALS
      * absolute filepath of where to cache tokens.  User must have read/write access. 

2. Run project once to get the authorization URL.  Click through till you get the authorization code.  Copy this and add to value of key auth_code in GOOGLE_API_OAUTH2_CLIENT_CREDENTIALS.
3. Re-Run the project

Tested with Flutter project:
https://github.com/flutter/packages/tree/main/packages/google_sign_in/google_sign_in/example
Commit: d92cdf0a63858857452ea945d344a65d7a0029e3
