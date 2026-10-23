// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

namespace Lumen::Libs::Graphics {

// Candidate index only: callers retain tracker ownership and still validate the
// canonical lifetime, backing and dynamic ownership before admitting an access.
// Each physical identity has an augmented AVL tree. A reverse lifetime index
// removes only that owner's fragments; unrelated resources are never traversed
// during replacement or retirement. Duplicate physical fragments remain distinct.
class MemoryHandoffPhysicalIndex
{
public:
	struct Span { uint64_t identity, begin, end; };
	struct Work { uint64_t nodes = 0, rotations = 0; };

	bool Replace(uint64_t lifetime, const std::vector<Span>& spans)
	{
		if (lifetime == 0 || spans.empty()) return false;
		for (const auto& span: spans)
			if (span.identity == 0 || span.begin >= span.end) return false;
		// Validate the entire replacement before removing the previous candidate.
		Erase(lifetime);
		m_lifetimes.emplace(lifetime, spans);
		for (size_t ordinal = 0; ordinal < spans.size(); ++ordinal)
		{
			const auto& span = spans[ordinal];
			Insert(m_roots[span.identity], {span.begin, span.end, lifetime, ordinal});
			++m_size;
		}
		return true;
	}

	void Erase(uint64_t lifetime)
	{
		const auto found = m_lifetimes.find(lifetime);
		if (found == m_lifetimes.end()) return;
		for (size_t ordinal = 0; ordinal < found->second.size(); ++ordinal)
		{
			const auto& span = found->second[ordinal];
			auto root = m_roots.find(span.identity);
			Remove(root->second, {span.begin, span.end, lifetime, ordinal});
			if (!root->second) m_roots.erase(root);
			--m_size;
		}
		m_lifetimes.erase(found);
	}

	template <typename Visitor> bool Visit(uint64_t identity, uint64_t begin, uint64_t end, Visitor&& visitor) const
	{
		if (identity == 0 || begin >= end) return false;
		const auto root = m_roots.find(identity);
		return root == m_roots.end() || VisitNode(root->second.get(), begin, end, visitor);
	}

	void Clear() { m_roots.clear(); m_lifetimes.clear(); m_size = 0; }
	size_t Size() const { return m_size; }
	size_t ResourceCount() const { return m_lifetimes.size(); }
	Work TakeWork() { const auto result = m_work; m_work = {}; return result; }

	// Test/diagnostic oracle. Never called on the production query/update path.
	bool Validate() const
	{
		size_t total = 0;
		for (const auto& entry: m_roots)
		{
			const auto audit = Audit(entry.second.get(), nullptr, nullptr);
			if (!audit.valid || !entry.second || entry.first == 0) return false;
			total += audit.count;
		}
		size_t expected = 0;
		for (const auto& entry: m_lifetimes)
			for (size_t ordinal = 0; ordinal < entry.second.size(); ++ordinal)
			{
				const auto& span = entry.second[ordinal];
				const auto root = m_roots.find(span.identity);
				if (root == m_roots.end() || !Find(root->second.get(), {span.begin, span.end, entry.first, ordinal}))
					return false;
				++expected;
			}
		return total == expected && total == m_size;
	}

private:
	struct Key
	{
		uint64_t begin, end, lifetime, ordinal;
		bool operator<(const Key& other) const
		{
			return std::tie(begin, end, lifetime, ordinal) < std::tie(other.begin, other.end, other.lifetime, other.ordinal);
		}
	};
	struct Node
	{
		explicit Node(Key value): key(value), max_end(value.end) {}
		Key key;
		uint64_t max_end;
		int height = 1;
		std::unique_ptr<Node> left, right;
	};
	using Root = std::unique_ptr<Node>;
	static int Height(const Root& node) { return node ? node->height : 0; }
	static uint64_t MaxEnd(const Root& node) { return node ? node->max_end : 0; }
	static void Update(Node* node)
	{
		node->height = 1 + std::max(Height(node->left), Height(node->right));
		node->max_end = std::max(node->key.end, std::max(MaxEnd(node->left), MaxEnd(node->right)));
	}
	void RotateLeft(Root& root)
	{
		auto next = std::move(root->right);
		root->right = std::move(next->left);
		Update(root.get());
		next->left = std::move(root);
		Update(next.get());
		root = std::move(next);
		++m_work.rotations;
	}
	void RotateRight(Root& root)
	{
		auto next = std::move(root->left);
		root->left = std::move(next->right);
		Update(root.get());
		next->right = std::move(root);
		Update(next.get());
		root = std::move(next);
		++m_work.rotations;
	}
	void Balance(Root& root)
	{
		if (!root) return;
		Update(root.get());
		const int balance = Height(root->left) - Height(root->right);
		if (balance > 1)
		{
			if (Height(root->left->left) < Height(root->left->right)) RotateLeft(root->left);
			RotateRight(root);
		} else if (balance < -1)
		{
			if (Height(root->right->right) < Height(root->right->left)) RotateRight(root->right);
			RotateLeft(root);
		}
	}
	void Insert(Root& root, Key key)
	{
		++m_work.nodes;
		if (!root) { root = std::make_unique<Node>(key); return; }
		Insert(key < root->key ? root->left : root->right, key);
		Balance(root);
	}
	void Remove(Root& root, const Key& key)
	{
		++m_work.nodes;
		if (key < root->key) Remove(root->left, key);
		else if (root->key < key) Remove(root->right, key);
		else if (!root->left) { auto child = std::move(root->right); root = std::move(child); }
		else if (!root->right) { auto child = std::move(root->left); root = std::move(child); }
		else
		{
			const auto* successor = root->right.get();
			while (successor->left) { successor = successor->left.get(); ++m_work.nodes; }
			root->key = successor->key;
			Remove(root->right, root->key);
		}
		Balance(root);
	}
	template <typename Visitor> static bool VisitNode(const Node* node, uint64_t begin, uint64_t end, Visitor& visitor)
	{
		if (node == nullptr || node->max_end <= begin) return true;
		if (!VisitNode(node->left.get(), begin, end, visitor)) return false;
		if (node->key.begin >= end) return true;
		if (node->key.end > begin && !visitor(node->key.lifetime)) return false;
		return VisitNode(node->right.get(), begin, end, visitor);
	}
	static bool Find(const Node* node, const Key& key)
	{
		while (node != nullptr)
		{
			if (key < node->key) node = node->left.get();
			else if (node->key < key) node = node->right.get();
			else return true;
		}
		return false;
	}
	struct AuditResult { bool valid = true; int height = 0; uint64_t max_end = 0; size_t count = 0; };
	static AuditResult Audit(const Node* node, const Key* lower, const Key* upper)
	{
		if (node == nullptr) return {};
		const auto left = Audit(node->left.get(), lower, &node->key), right = Audit(node->right.get(), &node->key, upper);
		const int height = 1 + std::max(left.height, right.height);
		const auto max_end = std::max(node->key.end, std::max(left.max_end, right.max_end));
		return {left.valid && right.valid && (!lower || *lower < node->key) && (!upper || node->key < *upper) &&
		    node->key.begin < node->key.end && node->key.lifetime != 0 &&
		    left.height - right.height >= -1 && left.height - right.height <= 1 &&
		    node->height == height && node->max_end == max_end, height, max_end, 1 + left.count + right.count};
	}
	std::map<uint64_t, Root> m_roots;
	std::map<uint64_t, std::vector<Span>> m_lifetimes;
	size_t m_size = 0;
	Work m_work;
};

} // namespace Lumen::Libs::Graphics
