# Textpresso Cognito Hosted UI

This directory stores the source files used to brand the Cognito Hosted UI for Textpresso.

The live login page is served by the `Textpresso Users` Cognito user pool:

- User pool ID: `us-east-1_Wiw1ZWjlc`
- Hosted UI domain: `textpresso-auth.auth.us-east-1.amazoncognito.com`
- App client: `textpresso-alb-client`

## Files

- `textpresso-hosted-ui.css`: custom Cognito Hosted UI CSS.
- `alliance-logo.png`: source Alliance logo copied from the AI Curation Cognito page.
- `textpresso-login-logo.png`: deployed composite logo that adds the Textpresso name.

## Design Notes

The styling follows the AI Curation Cognito page: a centered white login panel, Alliance logo, restrained colors, and a single strong blue action button.

Cognito Hosted UI does not allow arbitrary HTML changes, and its CSS validator only allows a small set of Cognito-managed classes. The literal identity-provider copy and button value are emitted by Cognito, so normal CSS text replacement is not available.

To avoid the awkward `Sign in with your corporate ID` wording while still giving curators context, the CSS hides that description line and visually relabels the identity-provider button as `Alliance curator sign-in` with a small SVG background image. The email/password side is visually relabeled as `Textpresso account sign-in`.

The standard Cognito email/password fields and links are left available for local Cognito users.

## Deploy

Deploy the current CSS and logo with the `ctabone` AWS profile:

```bash
TEXPRESSO_COGNITO_CLIENT_ID=$(aws cognito-idp list-user-pool-clients \
  --profile ctabone \
  --region us-east-1 \
  --user-pool-id us-east-1_Wiw1ZWjlc \
  --query "UserPoolClients[?ClientName=='textpresso-alb-client'].ClientId | [0]" \
  --output text)

aws cognito-idp set-ui-customization \
  --profile ctabone \
  --region us-east-1 \
  --user-pool-id us-east-1_Wiw1ZWjlc \
  --client-id "$TEXPRESSO_COGNITO_CLIENT_ID" \
  --css file://docs/cognito-hosted-ui/textpresso-hosted-ui.css \
  --image-file fileb://docs/cognito-hosted-ui/textpresso-login-logo.png
```

Set the customization on the app client, not only the pool default. The classic Cognito login HTML did not inject the logo from the pool-default `ALL` customization during testing, but it did inject the client-specific logo.

Check the deployed customization:

```bash
aws cognito-idp get-ui-customization \
  --profile ctabone \
  --region us-east-1 \
  --user-pool-id us-east-1_Wiw1ZWjlc \
  --client-id "$TEXPRESSO_COGNITO_CLIENT_ID"
```
