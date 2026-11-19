# Changelog

## 1.0.0

First release of the Web API Testing Toolkit (watt) for Windows 10 and 11 (64-bit).

- REST over HTTP/1.1 and HTTP/2, gRPC (all four call types, reflection, `.proto` files, gRPC-Web),
  WebSocket, Server-Sent Events and GraphQL.
- Elapsed time on every response, with a phase waterfall, connection reuse, TLS and certificate
  details.
- Workspaces as folders of JSON files with folder and collection inheritance, environments,
  encrypted secrets, and history with full responses.
- Basic, Bearer, Digest, NTLM, Negotiate, AWS Signature v4 and OAuth 2.0 authentication; client
  certificates, custom CAs and proxies.
- Assertions, captures, and pre-request / post-response scripts with the Postman and Bruno APIs.
- Collection runner with data files, load profiles and latency percentiles, also from the command
  line (`watt --run`) with JUnit, JSON and HTML reports.
- Mock server from saved example responses.
- Import from Postman, OpenAPI and curl; copy as curl and as code in eight languages.
