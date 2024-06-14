# Symbolic links

This section provides information on [symbolic links](https://{{lang}}.wikipedia.org/wiki/Symbolic_link).

## General information { #common }

The system supports `soft links` in [Cypress](../../../user-guide/storage/cypress.md). This link is a node of type `link` referencing another `target` object.
The path to it is specified at link creation time. While the address is being computed for the operation, all attempts to access the link node are automatically redirected to the target object.

A link to a target object depends on its path.
If a target path is deleted or moved, the link will break. You will see an error message saying `Link target does not exist`.

A link node does not take up space on disk. Its `resource_usage/disk_space` attribute is equal to 0. Therefore, creating links cannot cause you to exceed your disk quota.
However, each link is actually a separate `node`, so it's possible to exceed your account's limit on the number of `nodes`.

## Creating { #create }

You can create a symbolic link with the `link` [command](../../../api/commands.md#link). Use the path to the target node and the path to the link node in that order as arguments:

CLI
```bash
yt link //tmp/target_table //tmp/target_table_link
```

A link may reference arbitrary nodes and objects.
Such as the `//tmp` subtree in the home directory:

CLI
```bash
yt link //tmp //home/username/my_own_tmp
```

{% note info "Note" %}

Relative links are not supported.

{% endnote %}

## Redirects { #redirects }

Automatic redirects from a link node to a target object have a number of exceptions:

- The `remove`, `create`, and `link` commands for the target path are processed in the link node. When deleting `//tmp/target_table_link`, for example, a user will not damage `//tmp/target_table`.
- The `move` command will affect the target node invalidating the link node.

You can also disable redirection by appending the `&` (ampersand) marker to the end of the link name.
This will help you explore the attributes of a link node, for example, rather than those of a target object.
Thus, to retrieve the target object path, run `yt get //tmp/target_table_link&/@`. You will get the path through the `target_path` attribute.

## System attributes { #attributes }

In addition to [attributes](../../../user-guide/storage/attributes.md) common to all nodes, link nodes also have the following properties:

| **Name** | **Type** | **Value** |
| ------------- | -------- | ------------------------------------------------------------ |
| `target_path` | `string` | Path to the object referenced by a link. |
| `broken` | `bool` | `true` if a link references a non-existent object, `false` otherwise. |

