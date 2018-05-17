#!/usr/bin/python
# Copyright (c) 2017 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Generates certificate chains where the intermediate contains netscape server
gated crypto rather than serverAuth."""

import sys
sys.path += ['../..']

import gencerts

# TODO(eroman): Update the defaults to be unexpired instead of explicitly
# setting in this test (which is being used by cert_verify_proc_unittest.cc).
gencerts.set_default_validity_range('180101120000Z', '240101120000Z')

def generate_chain(intermediate_digest_algorithm):
  # Self-signed root certificate.
  root = gencerts.create_self_signed_root_certificate('Root')

  # Intermediate certificate.
  intermediate = gencerts.create_intermediate_certificate('Intermediate', root)
  intermediate.set_signature_hash(intermediate_digest_algorithm)
  intermediate.get_extensions().set_property('extendedKeyUsage',
                                             'nsSGC')

  # Target certificate.
  target = gencerts.create_end_entity_certificate('Target', intermediate)
  target.get_extensions().set_property('extendedKeyUsage',
                                   'serverAuth,clientAuth')
  # TODO(eroman): Set subjectAltName by default rather than specifically in
  # this test.
  target.get_extensions().set_property('subjectAltName', 'DNS:test.example')

  chain = [target, intermediate, root]
  gencerts.write_chain(__doc__, chain,
                       '%s-chain.pem' % intermediate_digest_algorithm)

# Generate two chains, whose only difference is the digest algorithm used for
# the intermediate's signature.
for digest in ['sha1', 'sha256']:
  generate_chain(digest)
