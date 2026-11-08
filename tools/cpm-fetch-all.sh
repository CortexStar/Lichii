#!/bin/sh -e

# SPDX-FileCopyrightText: Copyright 2025 Lichii Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2025 crueter
# SPDX-License-Identifier: GPL-3.0-or-later

# provided for workflow compat

find tools/ -name '*.sh' -type f -exec chmod +x {} \;

# shellcheck disable=SC2086
tools/cpmutil.sh package fetch -a
