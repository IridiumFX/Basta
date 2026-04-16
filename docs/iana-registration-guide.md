# IANA Media Type Registration Guide

Step-by-step guide for registering `text/vnd.pasta` and `application/vnd.basta`
with IANA.

---

## Prerequisites

Before submitting, ensure:

- [ ] Both GitHub repos are **public** (the spec files must be accessible)
  - Pasta: `specs/Pasta.txt`
  - Basta: `specs/Basta.txt`
- [ ] Contact name and email in both templates are correct
  - Edit `docs/iana-media-type.txt` in each repo if needed
- [ ] Specs are up to date (dot-in-labels, sections flag, blob format)

---

## Step 1: Go to the IANA registration page

Navigate to the IANA Media Types page and find the
**"Application for a Media Type"** link. There are two forms depending on
the type tree:

- **Vendor tree** (`vnd.*`) — this is what we're using. No RFC required.
- Standards tree — requires an RFC and IETF review (not needed now).

---

## Step 2: Submit Pasta registration

Fill in the online form using the contents of:

    Pasta/docs/iana-media-type.txt

Key fields:

| Field                  | Value                          |
|------------------------|--------------------------------|
| Type name              | `text`                         |
| Subtype name           | `vnd.pasta`                    |
| Encoding               | 8bit (pure text)               |
| File extension         | `.pasta`                       |
| Intended usage         | COMMON                         |

---

## Step 3: Submit Basta registration

Fill in the online form using the contents of:

    Basta/docs/iana-media-type.txt

Key fields:

| Field                  | Value                          |
|------------------------|--------------------------------|
| Type name              | `application`                  |
| Subtype name           | `vnd.basta`                    |
| Encoding               | Binary (due to blob payloads)  |
| File extension         | `.basta`                       |
| Intended usage         | COMMON                         |

---

## Step 4: Wait for review

- IANA reviews vendor-tree registrations internally (no public review).
- Typical turnaround: **2-4 weeks**.
- IANA may email follow-up questions to the contact address.
- No fee.

---

## Step 5: Confirm registration

Once approved:

- [ ] Verify both types appear in the IANA Media Types registry
- [ ] Update both `docs/guide.md` files to reference the official media types
- [ ] Consider adding the media type to any HTTP servers or tools that serve
      these files (e.g., `Content-Type: text/vnd.pasta` or
      `Content-Type: application/vnd.basta`)

---

## Future: Standards tree (optional)

If the formats gain wider adoption, you can migrate from `vnd.*` to the
standards tree (`text/pasta`, `application/basta`). This requires:

1. Writing an RFC (Internet-Draft → IETF review → publication)
2. IETF expert review of the media type
3. Significantly more effort — only worth it if the formats become
   widely adopted beyond your own projects

The vendor tree registrations are permanent and perfectly fine for
production use. No urgency to move to the standards tree.
