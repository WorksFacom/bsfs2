"""
Allocator with a lazy model using a B+Tree for recycled blocks.

Strategy idea:
- use a B+Tree to keep recycled blocks ordered, with fast minimum access via
  `first_leaf`.
- combine this recycled minimum with fresh range (`next_fresh`).

How `next_fresh` works:
- indicates the smallest index still never used.
- is consumed in increasing order when it wins against recycled minimum.
- deallocations return to B+Tree and do not move cursor backwards.

Allocation:
- compares `next_fresh` with the recycled minimum in `first_leaf` (first B+Tree
  leaf) and chooses the smallest index.
- when consuming recycled index, removes it from B+Tree; when consuming fresh,
  advances cursor.
- marks selected indices as occupied in the bitmap.

Deallocation:
- validates indices, frees blocks in the bitmap, and reinserts into B+Tree.
- keeps `first_leaf` consistent after removals/insertions to preserve fast
  access to the smallest recycled index.
"""

import bisect
from core import BaseAllocator, register


class BPNode:
    """B+Tree node"""
    
    def __init__(self, order, is_leaf=True):
        self.order = order
        self.is_leaf = is_leaf
        self.keys = []
        self.children = []
        self.parent = None
        self.next = None
        self.prev = None
    
    def is_full(self):
        return len(self.keys) >= 2 * self.order - 1


@register("bplustree", display_name="B+Tree")
class BitmapBPlusTree(BaseAllocator):
    """B+Tree indexed Bitmap Allocator"""
    
    def __init__(self, size, order=256):
        """Initialize bitmap with B+Tree indexing"""
        super().__init__(size)
        self.order = order
        self.root = BPNode(order, is_leaf=True)
        self.first_leaf = self.root
        self.next_fresh = 0
    
    def find_free_block(self):
        """Find minimum free index among recycled B+Tree and fresh range."""
        tree_min = -1
        leaf = self.first_leaf
        if leaf and leaf.keys:
            tree_min = leaf.keys[0]
        fresh_min = self.next_fresh if self.next_fresh < self.size else -1
        if tree_min == -1:
            return fresh_min
        if fresh_min == -1:
            return tree_min
        return min(tree_min, fresh_min)
    
    def allocate_blocks(self, count):
        """Allocate count free blocks"""
        if count <= 0:
            return []
        if self.free_blocks_remaining < count:
            return []

        allocated = []

        for _ in range(count):
            idx = self.find_free_block()
            if idx == -1:
                return []

            self.bitmap[idx] = 1
            if idx == self.next_fresh:
                self.next_fresh += 1
            else:
                self._delete_key(idx)
            allocated.append(idx)

        return allocated
    
    def deallocate_blocks(self, indices):
        """Deallocate blocks"""
        if not self.validate_deallocation(indices):
            return False

        for idx in indices:
            self.bitmap[idx] = 0
            self._insert_key(idx)
        return True
    
    def count_free_blocks(self):
        """Count free blocks."""
        return self.free_blocks_remaining
    
    # ===== B+Tree Operations =====
    
    def _insert_key(self, key):
        """Insert key into B+Tree"""
        if self.root.is_full():
            new_root = BPNode(self.order, is_leaf=False)
            new_root.children.append(self.root)
            self.root.parent = new_root
            self._split_child(new_root, 0)
            self.root = new_root
        
        self._insert_non_full(self.root, key)
    
    def _insert_non_full(self, node, key):
        """Insert into non-full node"""
        if node.is_leaf:
            i = bisect.bisect_left(node.keys, key)
            node.keys.insert(i, key)
        else:
            i = bisect.bisect_left(node.keys, key)

            child = node.children[i]
            if child.is_full():
                self._split_child(node, i)
                if key > node.keys[i]:
                    i += 1
                child = node.children[i]

            self._insert_non_full(child, key)
    
    def _split_child(self, parent, index):
        """Split full child"""
        full_child = parent.children[index]
        new_child = BPNode(self.order, full_child.is_leaf)
        
        mid_index = self.order - 1
        
        if full_child.is_leaf:
            new_child.keys = full_child.keys[mid_index:]
            full_child.keys = full_child.keys[:mid_index]
            
            new_child.next = full_child.next
            new_child.prev = full_child
            if full_child.next:
                full_child.next.prev = new_child
            full_child.next = new_child
            
            parent.keys.insert(index, new_child.keys[0])
        else:
            promoted_key = full_child.keys[mid_index]
            new_child.keys = full_child.keys[mid_index + 1:]
            new_child.children = full_child.children[mid_index + 1:]
            
            full_child.keys = full_child.keys[:mid_index]
            full_child.children = full_child.children[:mid_index + 1]
            
            for child in new_child.children:
                child.parent = new_child
            
            parent.keys.insert(index, promoted_key)
        
        parent.children.insert(index + 1, new_child)
        new_child.parent = parent
    
    def _delete_key(self, key):
        """Delete key from B+Tree"""
        self._delete_from_node(self.root, key)

        if not self.root.keys and self.root.children:
            self.root = self.root.children[0]
            self.root.parent = None

        # Smart first_leaf update: only recalculate if minimum was deleted
        if self.first_leaf and self.first_leaf.keys and key == self.first_leaf.keys[0]:
            self.first_leaf = self.first_leaf.next if self.first_leaf.next else self.first_leaf
        elif self.first_leaf and not self.first_leaf.keys and self.first_leaf.next:
            self.first_leaf = self.first_leaf.next
        elif not self.first_leaf or not self.first_leaf.keys:
            curr = self.root
            while not curr.is_leaf:
                curr = curr.children[0]
            self.first_leaf = curr
    
    def _delete_from_node(self, node, key):
        """Delete key from node"""
        i = bisect.bisect_left(node.keys, key)
        
        if node.is_leaf:
            if i < len(node.keys) and node.keys[i] == key:
                node.keys.pop(i)
        else:
            if i < len(node.keys) and node.keys[i] == key:
                child = node.children[i]
                if len(child.keys) >= self.order:
                    predecessor = child.keys[-1]
                    node.keys[i] = predecessor
                    self._delete_from_node(child, predecessor)
                else:
                    sibling = node.children[i + 1]
                    child.keys.append(node.keys[i])
                    child.keys.extend(sibling.keys)
                    
                    if not child.is_leaf:
                        child.children.extend(sibling.children)
                        for c in sibling.children:
                            c.parent = child
                    else:
                        child.next = sibling.next
                        if sibling.next:
                            sibling.next.prev = child
                    
                    node.keys.pop(i)
                    node.children.pop(i + 1)
                    self._delete_from_node(child, key)
            else:
                child = node.children[i]
                if len(child.keys) < self.order:
                    self._fill(node, i)
                
                if i > len(node.keys):
                    self._delete_from_node(node.children[i - 1], key)
                else:
                    self._delete_from_node(node.children[i], key)
    
    def _fill(self, node, index):
        """Fill child with minimum keys"""
        if index > 0 and len(node.children[index - 1].keys) >= self.order:
            self._borrow_from_prev(node, index)
        elif index < len(node.children) - 1 and len(node.children[index + 1].keys) >= self.order:
            self._borrow_from_next(node, index)
        else:
            if index < len(node.children) - 1:
                self._merge(node, index)
            else:
                self._merge(node, index - 1)
    
    def _borrow_from_prev(self, node, child_index):
        """Borrow from previous sibling"""
        child = node.children[child_index]
        sibling = node.children[child_index - 1]
        
        child.keys.insert(0, sibling.keys.pop())
        
        if not child.is_leaf:
            child.children.insert(0, sibling.children.pop())
            child.children[0].parent = child
        
        node.keys[child_index - 1] = child.keys[0]
    
    def _borrow_from_next(self, node, child_index):
        """Borrow from next sibling"""
        child = node.children[child_index]
        sibling = node.children[child_index + 1]
        
        child.keys.append(sibling.keys.pop(0))
        
        if not child.is_leaf:
            child.children.append(sibling.children.pop(0))
            child.children[-1].parent = child
        
        node.keys[child_index] = sibling.keys[0]
    
    def _merge(self, node, index):
        """Merge child with sibling"""
        child = node.children[index]
        sibling = node.children[index + 1]
        
        if child.is_leaf:
            child.keys.extend(sibling.keys)
            child.next = sibling.next
            if sibling.next:
                sibling.next.prev = child
        else:
            child.keys.append(node.keys[index])
            child.keys.extend(sibling.keys)
            child.children.extend(sibling.children)
            for c in sibling.children:
                c.parent = child
        
        node.keys.pop(index)
        node.children.pop(index + 1)
